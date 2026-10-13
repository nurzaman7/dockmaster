/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * A co-processor link on USART1 (metered, registered by its driver at
 * boot) next to a local radio, played here by a fake link because the
 * Nucleo board has none. Producers submit C0 control, C1 state and C4 bulk;
 * every few seconds the sample prints the counters. Expected with the
 * default policy: C4 never uses the metered link, C0 goes out on both.
 *
 * Without a co-processor on the pins the NCP link times out (watchdog
 * faults), degrades and goes down, and everything moves to the local
 * radio. Not run on hardware yet.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <dockmaster/dm.h>
#include <dockmaster/dm_link_fake.h>

#define TICK_MS   50U
#define REPORT_MS 5000U

static struct dm_stats s;

static void report(void)
{
	static const char *const states[] = {"DOWN", "DEGRADED", "UP"};

	dm_stats_get(&s);
	for (int c = DM_C0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_stats *k = &s.cls[c];

		printk("C%d submitted %u sent %u failed %u dropped %u coalesced %u\n", c,
		       k->submitted, k->sent, k->failed,
		       k->dropped_quota + k->dropped_age + k->dropped_shed, k->coalesced);
	}
	for (uint8_t l = 0; l < dm_link_count(); l++) {
		const struct dm_link_stats *k = &s.link[l];

		printk("link %u %-8s tx_ok %u tx_fail %u faults %u probes %u/%u rtt %u us\n", l,
		       states[k->state], k->tx_ok, k->tx_fail, k->faults, k->probes_ok,
		       k->probes_fail, k->ewma_rtt_us);
	}
	printk("pressure %u, slab %u%%, c0_lost %u\n\n", s.pressure_state, s.slab_pct,
	       s.c0_lost);
}

int main(void)
{
	const struct dm_fake_cfg radio = {
		.caps = {.mtu = 127, .tier = DM_TIER_MAC, .mac_prio_levels = 2},
		.bitrate_bps = 250000,
		.overhead_us = 800,
		.airtime_latency = true,
		.seed = 1,
	};
	uint8_t buf[64];
	uint8_t link;
	uint32_t seq = 0U;

	dm_fake_configure(0, &radio);
	if (dm_fake_register(0, &link) != 0) {
		printk("local radio registration failed\n");
		return 0;
	}
	printk("ncp_uart_link: %u link(s), local radio is link %u\n", dm_link_count(), link);

	for (uint32_t t = 0U;; t += TICK_MS) {
		memset(buf, 0x5A, sizeof(buf));
		sys_put_le32(seq++, buf);
		(void)dm_submit(DM_C4, buf, sizeof(buf), DM_F_RESUMABLE, NULL);
		if (t % 1000U == 0U) {
			(void)dm_submit(DM_C0, buf, 8, 0U, NULL);
		}
		if (t % 200U == 0U) {
			struct dm_submit_opts o = {.coalesce_key = 1, .link_hint = DM_LINK_ANY};

			(void)dm_submit(DM_C1, buf, 16, DM_F_COALESCE, &o);
		}
		if (t % REPORT_MS == 0U && t != 0U) {
			report();
		}
		k_msleep(TICK_MS);
	}
	return 0;
}
