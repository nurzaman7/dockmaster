/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * DM_TIER_HW backend for nRF52 (PPI) and nRF53 (DPPI), spec section 11.
 *
 * Two class lines, each a chain set up once, at the first bind:
 *
 *   radio output -> SoC pin -> GPIOTE IN[ch] -> (D)PPI -> EGU[n].TASKS_TRIGGER[0]
 *   -> EGU[n] interrupt
 *
 * Line 0 serves C0 to C2 at DM_IRQ_HW_C0, line 1 serves C3 and C4 at
 * DM_IRQ_HW_C4, so a C0 completion preempts a C4 completion handler instead
 * of queueing behind it. nrfx_gppi hides the PPI/DPPI difference, so one
 * code path serves both families. The GPIOTE channel's own interrupt stays
 * disabled: GPIOTE_IRQn never fires for a class line, only the EGU does.
 *
 * The devicetree node (dockmaster,hw-nrf-egu) fixes the EGU instance, the
 * GPIOTE channel and the SoC pin of each line. The (D)PPI channel comes
 * from the nrfx_gppi allocator so it cannot collide with a radio driver or
 * a Bluetooth controller.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_nrf.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <cmsis_core.h>
#include <hal/nrf_egu.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>

#include <dockmaster/dm_hw.h>

#include "../dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

#if !defined(CONFIG_NRFX_GPPI) || !defined(CONFIG_GPIO_NRFX_INTERRUPT)
#error "CONFIG_DM_TIER_HW_NRF needs CONFIG_NRFX_GPPI=y and CONFIG_GPIO=y"
#endif

#define HW_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(dockmaster_hw_nrf_egu)
#define N_LINES 2U

#define LINE_EGU_NR(i) DT_PROP_BY_IDX(HW_NODE, egu_instances, i)
#define LINE_CH(i)     DT_PROP_BY_IDX(HW_NODE, gpiote_channels, i)
#define EGU_NODE(i)    DT_NODELABEL(UTIL_CAT(egu, LINE_EGU_NR(i)))

BUILD_ASSERT(DT_PROP_LEN(HW_NODE, egu_instances) == N_LINES &&
		     DT_PROP_LEN(HW_NODE, gpiote_channels) == N_LINES &&
		     DT_PROP_LEN(HW_NODE, line_gpios) == N_LINES,
	     "dockmaster,hw-nrf-egu: every property needs exactly two entries");
BUILD_ASSERT(LINE_EGU_NR(0) != LINE_EGU_NR(1) && LINE_CH(0) != LINE_CH(1),
	     "dockmaster,hw-nrf-egu: the two lines need distinct EGUs and GPIOTE channels");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(EGU_NODE(0)) && DT_NODE_HAS_STATUS_OKAY(EGU_NODE(1)),
	     "dockmaster,hw-nrf-egu: EGU instance missing or disabled");

struct dm_hw_line {
	NRF_EGU_Type *egu;
	struct gpio_dt_spec pin;
	uint32_t psel; /* absolute pin number */
	nrfx_gpiote_trigger_t trigger;
	uint16_t irqn;
	uint8_t prio;
	uint8_t egu_nr;
	uint8_t ch;
	/* Run time. */
	bool routed;
	uint8_t link;         /* the one link this line completes, DM_NO_LINK if none */
	volatile uint8_t cls; /* class of the frame in flight on this line */
	atomic_t fired;       /* completions seen, for labs and debugging */
};

#define LINE_INIT(i, level)                                                                 \
	{                                                                                   \
		.egu = (NRF_EGU_Type *)DT_REG_ADDR(EGU_NODE(i)),                            \
		.pin = GPIO_DT_SPEC_GET_BY_IDX(HW_NODE, line_gpios, i),                     \
		.psel = NRF_DT_GPIOS_TO_PSEL_BY_IDX(HW_NODE, line_gpios, i),                \
		.trigger = (DT_GPIO_FLAGS_BY_IDX(HW_NODE, line_gpios, i) & GPIO_ACTIVE_LOW) \
				   ? NRFX_GPIOTE_TRIGGER_HITOLO                             \
				   : NRFX_GPIOTE_TRIGGER_LOTOHI,                            \
		.irqn = DT_IRQN(EGU_NODE(i)),                                               \
		.prio = DM_IRQ_PRIO_CONST(level),                                           \
		.egu_nr = LINE_EGU_NR(i),                                                   \
		.ch = LINE_CH(i),                                                           \
		.link = DM_NO_LINK,                                                         \
	}

static struct dm_hw_line lines[N_LINES] = {
	LINE_INIT(0, DM_IRQ_HW_C0),
	LINE_INIT(1, DM_IRQ_HW_C4),
};

static bool irqs_connected;

static inline uint8_t line_of(dm_class_t c)
{
	return c <= DM_C2 ? 0U : 1U;
}

/* Section 11 ISR rules: read and clear the event, DSB, complete. No logging. */
static inline void line_isr(struct dm_hw_line *ln)
{
	nrf_egu_event_clear(ln->egu, NRF_EGU_EVENT_TRIGGERED0);
	__DSB();
	atomic_inc(&ln->fired);
	DM_TRACE("dm_hw_line", ln->cls, ln->link, ln == &lines[0] ? 0U : 1U);
	if (ln->link != DM_NO_LINK) {
		/* The class line only says "done": status 0, airtime measured by the core. */
		dm_link_tx_done(ln->link, 0U, 0, 0U);
	}
}

ISR_DIRECT_DECLARE(dm_hw_line0_isr)
{
	line_isr(&lines[0]);
	ISR_DIRECT_PM();
	return 1; /* the owner thread may have become ready */
}

ISR_DIRECT_DECLARE(dm_hw_line1_isr)
{
	line_isr(&lines[1]);
	ISR_DIRECT_PM();
	return 1;
}

static void connect_irqs(void)
{
	IRQ_DIRECT_CONNECT(DT_IRQN(EGU_NODE(0)), DM_IRQ_PRIO_CONST(DM_IRQ_HW_C0), dm_hw_line0_isr,
			   0);
	IRQ_DIRECT_CONNECT(DT_IRQN(EGU_NODE(1)), DM_IRQ_PRIO_CONST(DM_IRQ_HW_C4), dm_hw_line1_isr,
			   0);
	irqs_connected = true;
}

/*
 * nrfx cannot reserve one particular GPIOTE channel, so allocate until the
 * wanted one comes up and hand the others back. Afterwards the GPIO driver
 * can never be given this channel for an ordinary pin interrupt.
 */
static int claim_channel(nrfx_gpiote_t *g, uint8_t want)
{
	uint32_t others = 0U;
	uint8_t ch;
	int rc;

	while ((rc = nrfx_gpiote_channel_alloc(g, &ch)) == 0 && ch != want) {
		others |= BIT(ch);
	}
	for (ch = 0U; others != 0U; ch++) {
		if ((others & BIT(ch)) != 0U) {
			(void)nrfx_gpiote_channel_free(g, ch);
			others &= ~BIT(ch);
		}
	}
	return rc == 0 ? 0 : -EBUSY;
}

static int route(struct dm_hw_line *ln)
{
	static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_NOPULL;
	const nrfx_gpiote_trigger_config_t trig = {
		.trigger = ln->trigger,
		.p_in_channel = &ln->ch,
	};
	const nrfx_gpiote_input_pin_config_t in = {
		.p_pull_config = &pull,
		.p_trigger_config = &trig,
		.p_handler_config = NULL,
	};
	nrfx_gpiote_t *g;
	nrfx_gppi_handle_t h;
	int rc;

	if (!gpio_is_ready_dt(&ln->pin)) {
		return -ENODEV;
	}
	g = gpio_nrf_gpiote_by_port_get(ln->pin.port);
	if (g == NULL) {
		return -ENOTSUP;
	}
	rc = claim_channel(g, ln->ch);
	if (rc != 0) {
		LOG_ERR("GPIOTE channel %u already in use", ln->ch);
		return rc;
	}
	rc = nrfx_gpiote_input_configure(g, ln->psel, &in);
	if (rc == 0) {
		/* Event only, for (D)PPI: the GPIOTE interrupt stays off. */
		nrfx_gpiote_trigger_enable(g, ln->psel, false);
		rc = nrfx_gppi_conn_alloc(nrfx_gpiote_in_event_address_get(g, ln->psel),
					  nrf_egu_task_address_get(ln->egu, NRF_EGU_TASK_TRIGGER0),
					  &h);
	}
	if (rc != 0) {
		(void)nrfx_gpiote_channel_free(g, ln->ch);
		LOG_ERR("EGU%u line: routing failed (%d)", ln->egu_nr, rc);
		return rc;
	}
	nrf_egu_event_clear(ln->egu, NRF_EGU_EVENT_TRIGGERED0);
	nrf_egu_int_enable(ln->egu, NRF_EGU_INT_TRIGGERED0);
	irq_enable(ln->irqn);
	nrfx_gppi_conn_enable(h);
	ln->routed = true;
	return 0;
}

int dm_hw_bind_class_line(uint8_t link, dm_class_t cls, unsigned int egu_idx,
			  unsigned int gpiote_ch)
{
	struct dm_hw_line *ln;
	int rc;

	if (link >= dm_n_links || (unsigned int)cls >= DM_CLASS_COUNT) {
		return -EINVAL;
	}
	ln = &lines[line_of(cls)];
	if (egu_idx != ln->egu_nr || gpiote_ch != ln->ch) {
		LOG_ERR("C%d: EGU%u/GPIOTE %u is not line %u of the devicetree node", cls,
			egu_idx, gpiote_ch, line_of(cls));
		return -EINVAL;
	}
	if (ln->link != DM_NO_LINK && ln->link != link) {
		return -EBUSY; /* one line completes exactly one link */
	}
	if (!irqs_connected) {
		connect_irqs();
	}
	if (!ln->routed) {
		rc = route(ln);
		if (rc != 0) {
			return rc;
		}
	}
	ln->link = link;
	dm_links[link].hw_irqn[cls] = ln->irqn;
	dm_links[link].hw_prio[cls] = ln->prio;
	return 0;
}

/* Owner thread, before every tx on a DM_TIER_HW link. */
int dm_hw_prepare(struct dm_link *l, dm_class_t c)
{
	if (l->hw_irqn[c] == 0U || l->ops->bind_class_irq == NULL) {
		return 0; /* no line for this class: the driver's own completion path */
	}
	lines[line_of(c)].cls = (uint8_t)c;
	return l->ops->bind_class_irq(l->dev, c, l->hw_irqn[c], l->hw_prio[c]);
}
