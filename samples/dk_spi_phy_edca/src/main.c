/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware lab sample (spec section 15, labs 1 to 3) for one external PHY
 * on SPI. A bulk producer keeps C4 busy at a fixed period, a control
 * producer injects one C0 message per CONFIG_APP_C0_PERIOD_MS, and every
 * CONFIG_APP_REPORT_PERIOD_S the sample prints the Dockmaster counters and
 * the thread analyzer's stack report. With lab3.overlay it also pulses a
 * stimulus pin that drives both tier 0 class lines at once.
 *
 * This sample builds in CI but has not been run on hardware yet.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>

#include <dockmaster/dm.h>
#include <dockmaster/dm_link.h>

#define TICK_MS     CONFIG_APP_C4_PERIOD_MS
#define REPORT_MS   (CONFIG_APP_REPORT_PERIOD_S * 1000U)
#define STIM_MS     100U

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), lab3_stim_gpios)
#define HAVE_STIM 1
static const struct gpio_dt_spec stim = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), lab3_stim_gpios);
#endif

/* Static: a dm_stats snapshot is several hundred bytes. */
static struct dm_stats s;

static void report(void)
{
	static const dm_class_t shown[] = {DM_C0, DM_C4};

	dm_stats_get(&s);
	for (size_t i = 0; i < ARRAY_SIZE(shown); i++) {
		const struct dm_class_stats *k = &s.cls[shown[i]];

		printk("C%d submitted %u refused %u sent %u failed %u shed %u airtime %u ms\n",
		       shown[i], k->submitted, k->refused, k->sent, k->failed, k->dropped_shed,
		       k->airtime_ms_used);
	}
	for (uint8_t l = 0; l < dm_link_count(); l++) {
		const struct dm_link_stats *k = &s.link[l];

		printk("link %u tier %u: tx_ok %u tx_fail %u faults %u edca_wait %u ms\n", l,
		       k->cur_tier, k->tx_ok, k->tx_fail, k->faults, k->busy_wait_ms);
	}
	printk("scheduler rounds %u, longest %u us, c0_lost %u\n", s.sched_rounds,
	       s.sched_max_us, s.c0_lost);
	thread_analyzer_print(0);
}

int main(void)
{
	uint8_t c4[CONFIG_APP_C4_LEN];
	uint8_t c0[8] = {0};
	uint32_t seq = 0U;

	if (dm_link_count() == 0U) {
		printk("no link registered: check the PHY node and the SPI bus\n");
		return 0;
	}
#ifdef HAVE_STIM
	if (gpio_pin_configure_dt(&stim, GPIO_OUTPUT_INACTIVE) != 0) {
		printk("stimulus pin unavailable\n");
		return 0;
	}
#endif
	printk("dk_spi_phy_edca: C4 every %u ms, C0 every %u ms\n", CONFIG_APP_C4_PERIOD_MS,
	       CONFIG_APP_C0_PERIOD_MS);

	for (uint32_t t = 0U;; t += TICK_MS) {
		memset(c4, 0xA5, sizeof(c4));
		sys_put_le32(seq++, c4);
		(void)dm_submit(DM_C4, c4, sizeof(c4), DM_F_RESUMABLE, NULL);

		if (t % CONFIG_APP_C0_PERIOD_MS < TICK_MS) {
			sys_put_le32(seq++, c0);
			(void)dm_submit(DM_C0, c0, sizeof(c0), 0U, NULL);
		}
#ifdef HAVE_STIM
		if (t % STIM_MS < TICK_MS) {
			/* One rising edge on both class lines: both EGU events in one cycle. */
			(void)gpio_pin_set_dt(&stim, 1);
			(void)gpio_pin_set_dt(&stim, 0);
		}
#endif
		if (t % REPORT_MS < TICK_MS && t != 0U) {
			report();
		}
		k_msleep(TICK_MS);
	}
	return 0;
}
