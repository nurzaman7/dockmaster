/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * External PHY on SPI (spec section 9): a generic command + FIFO
 * transceiver with a TX-done pin and an optional busy pin. No product is
 * targeted; a part with the command set below, or a thin adapter to it,
 * fits. One command per chip-select frame, SPI mode 0, MSB first:
 *
 *   0x01 WRITE_FIFO <len> <len bytes>  load the TX FIFO, replacing its contents
 *   0x02 START_TX                      send the FIFO; clears TX_DONE, which
 *                                      deasserts every done output
 *   0x03 ABORT                         stop any transmission, flush the FIFO;
 *                                      no TX_DONE follows
 *   0x04 MAP_IRQ <out>                 signal TX_DONE on PHY output <out>
 *                                      (0 = the done pin, the reset default)
 *
 * Assumptions, none of them checked on hardware:
 * - TX_DONE holds the mapped output asserted until START_TX or ABORT.
 * - A raw PHY cannot fail a frame once START_TX is accepted: done means the
 *   frame left the antenna. Delivery is the receiver's business.
 * - The busy output is asserted while the PHY senses energy (CCA or CAD).
 * - The PHY is half duplex: its receiver is off for the frame plus
 *   turnaround-us, and that time is reported as airtime.
 *
 * Nothing in interrupt context touches the bus. The done-pin ISR only
 * completes the transmission with dm_link_tx_done(); every SPI transfer
 * runs on the owner thread. For DM_TIER_HW the PHY routes each frame's
 * done signal to the output of its class line and the SoC backend
 * (subsys/dockmaster/hw) completes it from that line's own interrupt.
 */

#define DT_DRV_COMPAT dockmaster_spi_phy

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <dockmaster/dm_hw.h>
#include <dockmaster/dm_link.h>

LOG_MODULE_REGISTER(dm_spi_phy, CONFIG_DM_LOG_LEVEL);

#define CMD_WRITE_FIFO 0x01U
#define CMD_START_TX   0x02U
#define CMD_ABORT      0x03U
#define CMD_MAP_IRQ    0x04U

#define N_LINES   2U
#define LINE_NONE 0xFFU
#define OUT_DONE  0U

struct phy_cfg {
	struct spi_dt_spec bus;
	struct gpio_dt_spec done;
	struct gpio_dt_spec busy;
	struct dm_link_caps caps;
	uint32_t us_per_byte_q8; /* on-air time per byte in 1/256 us */
	uint32_t overhead_us;    /* preamble plus turnaround, per frame */
	uint32_t turnaround_us;
	uint8_t line_out[N_LINES]; /* PHY output of class line 0 and 1 */
	uint8_t hw_egu[N_LINES];   /* tier 0 routing, from the hw-backend node */
	uint8_t hw_ch[N_LINES];
};

struct phy_data {
	const struct device *dev;
	struct gpio_callback done_cb;
	atomic_t armed; /* a transmission waits for the done pin */
	uint32_t handle;
	uint32_t t0_cyc;
	uint8_t link;
	uint8_t next_line;  /* set by bind_class_irq, consumed by the next tx */
	uint8_t mapped_out; /* PHY output TX_DONE currently drives */
};

static uint8_t line_of(dm_class_t cls)
{
	return cls <= DM_C2 ? 0U : 1U;
}

/* One command frame: a header and an optional payload under one chip select. */
static int phy_cmd(const struct phy_cfg *cfg, const uint8_t *hdr, size_t hdr_len,
		   const uint8_t *data, size_t len)
{
	const struct spi_buf bufs[] = {
		{.buf = (uint8_t *)hdr, .len = hdr_len},
		{.buf = (uint8_t *)data, .len = len},
	};
	const struct spi_buf_set tx = {.buffers = bufs, .count = len != 0U ? 2U : 1U};

	return spi_write_dt(&cfg->bus, &tx);
}

static int phy_route(const struct device *dev, uint8_t out)
{
	const struct phy_cfg *cfg = dev->config;
	struct phy_data *d = dev->data;
	const uint8_t cmd[] = {CMD_MAP_IRQ, out};
	int rc;

	if (d->mapped_out == out) {
		return 0;
	}
	rc = phy_cmd(cfg, cmd, sizeof(cmd), NULL, 0U);
	if (rc == 0) {
		d->mapped_out = out;
	}
	return rc;
}

/* Pure: configuration only, 32-bit math (len <= 255, per-byte time bounded). */
static uint32_t phy_cost_of_us(const struct device *dev, size_t len)
{
	const struct phy_cfg *cfg = dev->config;

	return cfg->overhead_us + (((uint32_t)len * cfg->us_per_byte_q8 + 255U) >> 8);
}

static int phy_tx(const struct device *dev, const uint8_t *buf, size_t len, uint8_t mac_prio,
		  uint32_t *handle)
{
	static const uint8_t start = CMD_START_TX;
	const struct phy_cfg *cfg = dev->config;
	struct phy_data *d = dev->data;
	const uint8_t hdr[] = {CMD_WRITE_FIFO, (uint8_t)len};
	uint8_t line = d->next_line;
	int rc;

	ARG_UNUSED(mac_prio);
	d->next_line = LINE_NONE;
	if (len == 0U || len > cfg->caps.mtu) {
		return -EMSGSIZE;
	}
	/* No class line bound for this frame: TX_DONE goes back to the done pin. */
	rc = phy_route(dev, line == LINE_NONE ? OUT_DONE : cfg->line_out[line]);
	if (rc == 0) {
		rc = phy_cmd(cfg, hdr, sizeof(hdr), buf, len);
	}
	if (rc != 0) {
		return rc;
	}
	*handle = ++d->handle;
	d->t0_cyc = k_cycle_get_32();
	/* A class line completes through the SoC backend; the done pin stays quiet. */
	atomic_set(&d->armed, line == LINE_NONE ? 1 : 0);
	rc = phy_cmd(cfg, &start, 1U, NULL, 0U);
	if (rc != 0) {
		atomic_set(&d->armed, 0);
	}
	return rc;
}

static int phy_cancel(const struct device *dev, uint32_t handle)
{
	static const uint8_t abort_cmd = CMD_ABORT;
	struct phy_data *d = dev->data;

	ARG_UNUSED(handle);
	atomic_set(&d->armed, 0);
	return phy_cmd(dev->config, &abort_cmd, 1U, NULL, 0U);
}

static bool phy_is_busy(const struct device *dev)
{
	const struct phy_cfg *cfg = dev->config;

	return gpio_pin_get_dt(&cfg->busy) > 0;
}

/*
 * DM_TIER_HW: called by the owner thread right before tx for a class that
 * has its own line. The MAP_IRQ command goes out with that tx, so a frame
 * whose class has no line (no bind call) falls back to the done pin.
 */
static int phy_bind_class_irq(const struct device *dev, dm_class_t cls, unsigned int irqn,
			      unsigned int prio)
{
	const struct phy_cfg *cfg = dev->config;
	struct phy_data *d = dev->data;

	ARG_UNUSED(prio);
	if (cfg->caps.tier != DM_TIER_HW || irqn == 0U) {
		return -ENOTSUP;
	}
	d->next_line = line_of(cls);
	return 0;
}

static void phy_done_isr(const struct device *port, struct gpio_callback *cb,
			 gpio_port_pins_t pins)
{
	struct phy_data *d = CONTAINER_OF(cb, struct phy_data, done_cb);
	const struct phy_cfg *cfg = d->dev->config;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	if (!atomic_cas(&d->armed, 1, 0)) {
		return; /* not waiting: spurious edge, or cancelled */
	}
	dm_link_tx_done(d->link, d->handle, 0,
			k_cyc_to_us_floor32(k_cycle_get_32() - d->t0_cyc) + cfg->turnaround_us);
}

static const struct dm_link_ops phy_ops = {
	.tx = phy_tx,
	.cancel = phy_cancel,
	.cost_of_us = phy_cost_of_us,
	.bind_class_irq = phy_bind_class_irq,
};

static const struct dm_link_ops phy_ops_busy = {
	.tx = phy_tx,
	.cancel = phy_cancel,
	.cost_of_us = phy_cost_of_us,
	.is_busy = phy_is_busy,
	.bind_class_irq = phy_bind_class_irq,
};

static void phy_bind_lines(const struct device *dev)
{
	const struct phy_cfg *cfg = dev->config;
	struct phy_data *d = dev->data;

	for (int c = DM_C0; c < DM_CLASS_COUNT; c++) {
		uint8_t i = line_of((dm_class_t)c);
		int rc = dm_hw_bind_class_line(d->link, (dm_class_t)c, cfg->hw_egu[i],
					       cfg->hw_ch[i]);

		if (rc != 0) {
			LOG_WRN("%s: C%d has no class line (%d), done pin used", dev->name, c, rc);
		}
	}
}

static int phy_init(const struct device *dev)
{
	static const uint8_t abort_cmd = CMD_ABORT;
	static const uint8_t map_done[] = {CMD_MAP_IRQ, OUT_DONE};
	const struct phy_cfg *cfg = dev->config;
	struct phy_data *d = dev->data;
	bool has_busy = cfg->busy.port != NULL;
	int rc;

	d->dev = dev;
	d->next_line = LINE_NONE;
	if (!spi_is_ready_dt(&cfg->bus) || !gpio_is_ready_dt(&cfg->done) ||
	    (has_busy && !gpio_is_ready_dt(&cfg->busy))) {
		LOG_ERR("%s: bus or pins not ready", dev->name);
		return -ENODEV;
	}
	rc = gpio_pin_configure_dt(&cfg->done, GPIO_INPUT);
	if (rc == 0 && has_busy) {
		rc = gpio_pin_configure_dt(&cfg->busy, GPIO_INPUT);
	}
	if (rc == 0) {
		gpio_init_callback(&d->done_cb, phy_done_isr, BIT(cfg->done.pin));
		rc = gpio_add_callback_dt(&cfg->done, &d->done_cb);
	}
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&cfg->done, GPIO_INT_EDGE_TO_ACTIVE);
	}
	/* Known state: nothing in flight, TX_DONE on the done pin. */
	if (rc == 0) {
		rc = phy_cmd(cfg, &abort_cmd, 1U, NULL, 0U);
	}
	if (rc == 0) {
		rc = phy_cmd(cfg, map_done, sizeof(map_done), NULL, 0U);
	}
	if (rc == 0) {
		rc = dm_link_register(dev, has_busy ? &phy_ops_busy : &phy_ops, &cfg->caps,
				      &d->link);
	}
	if (rc != 0) {
		LOG_ERR("%s: init failed (%d)", dev->name, rc);
		return rc;
	}
	if (cfg->caps.tier == DM_TIER_HW) {
		phy_bind_lines(dev);
	}
	return 0;
}

#define PHY_TIER(n) DT_INST_ENUM_IDX_OR(n, tier, DM_TIER_SW)

/* Tier 0 routing (EGU instance, GPIOTE channel) comes from the backend node. */
#define PHY_HW(n, prop, i)                                                                  \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, hw_backend),                                   \
		    (DT_PROP_BY_IDX(DT_INST_PHANDLE(n, hw_backend), prop, i)), (0))

#define PHY_DEFINE(n)                                                                       \
	BUILD_ASSERT(DT_INST_PROP(n, mtu) >= 1 && DT_INST_PROP(n, mtu) <= 255,              \
		     "dockmaster,spi-phy: mtu must be 1 to 255");                           \
	BUILD_ASSERT(DT_INST_PROP(n, bitrate_bps) >= 128,                                   \
		     "dockmaster,spi-phy: bitrate-bps below 128 overflows cost_of_us");      \
	BUILD_ASSERT(PHY_TIER(n) != DM_TIER_MAC,                                            \
		     "dockmaster,spi-phy: the PHY has no MAC priority slots");              \
	BUILD_ASSERT(PHY_TIER(n) != DM_TIER_HW ||                                           \
			     (DT_INST_PROP_LEN_OR(n, class_line_pins, 0) == N_LINES &&      \
			      DT_INST_NODE_HAS_PROP(n, hw_backend)),                        \
		     "dockmaster,spi-phy: tier hw needs class-line-pins and hw-backend");    \
	static struct phy_data phy_data_##n;                                                \
	static const struct phy_cfg phy_cfg_##n = {                                         \
		.bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8) | SPI_TRANSFER_MSB |         \
						       SPI_OP_MODE_MASTER),                 \
		.done = GPIO_DT_SPEC_INST_GET(n, done_gpios),                               \
		.busy = GPIO_DT_SPEC_INST_GET_OR(n, busy_gpios, {0}),                       \
		.caps = {                                                                   \
			.mtu = DT_INST_PROP(n, mtu),                                        \
			.tier = PHY_TIER(n),                                                \
			.half_duplex = true,                                                \
			.metered = DT_INST_PROP(n, metered),                                \
			.probe_period_ms = DT_INST_PROP(n, probe_period_ms),                \
			.owner_prio = DT_INST_PROP_OR(n, owner_priority, 0),                \
			.owner_stack = DT_INST_PROP_OR(n, owner_stack, 0),                  \
		},                                                                          \
		.us_per_byte_q8 = (uint32_t)(2048000000ULL / DT_INST_PROP(n, bitrate_bps)), \
		.overhead_us = DT_INST_PROP(n, preamble_us) + DT_INST_PROP(n, turnaround_us), \
		.turnaround_us = DT_INST_PROP(n, turnaround_us),                            \
		.line_out = DT_INST_PROP_OR(n, class_line_pins, {0}),                       \
		.hw_egu = {PHY_HW(n, egu_instances, 0), PHY_HW(n, egu_instances, 1)},       \
		.hw_ch = {PHY_HW(n, gpiote_channels, 0), PHY_HW(n, gpiote_channels, 1)},    \
	};                                                                                  \
	DEVICE_DT_INST_DEFINE(n, phy_init, NULL, &phy_data_##n, &phy_cfg_##n, POST_KERNEL,  \
			      CONFIG_DM_LINK_SPI_PHY_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PHY_DEFINE)
