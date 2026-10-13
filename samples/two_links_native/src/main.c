/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two fake links: a lossy 250 kbit/s on-chip radio and a slow, metered
 * cellular modem. Five producers submit one class each for 12 s; the radio
 * fails for 3 s in the middle. At the end the sample checks the two
 * guarantees that matter most: every C0 message was delivered, and no bulk
 * (C4) byte went over the metered link.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <dockmaster/dm.h>
#include <dockmaster/dm_link_fake.h>

#define RADIO  0
#define CELL   1
#define RUN_MS 12000
#define C0_MAX 64

static uint8_t link_radio;
static uint8_t link_cell;
static ATOMIC_DEFINE(c0_seen, C0_MAX);
static atomic_t c4_cell_bytes;

static void on_tx(uint8_t fake, const uint8_t *buf, size_t len, int status)
{
	if (status != 0) {
		return;
	}
	if (buf[0] == DM_C0) {
		atomic_set_bit(c0_seen, sys_get_le32(&buf[1]) % C0_MAX);
	}
	if (buf[0] == DM_C4 && fake == CELL) {
		atomic_add(&c4_cell_bytes, (atomic_val_t)len);
	}
}

static void on_hint(dm_class_t cls, enum dm_pressure p)
{
	static const char *const names[] = {"NORMAL", "TIGHT", "SHED", "CRITICAL"};

	printk("hint: C%d boundary, pressure now %s\n", cls, names[p]);
}

static int submit(dm_class_t c, uint32_t seq, size_t len, uint32_t flags,
		  const struct dm_submit_opts *o)
{
	static uint8_t buf[DM_MAX_MSG]; /* one producer thread: main */

	memset(buf, 0x5A, len);
	buf[0] = (uint8_t)c;
	sys_put_le32(seq, &buf[1]);
	return dm_submit(c, buf, len, flags, o);
}

/* Static: a dm_stats snapshot is about 650 bytes, too much for a small stack. */
static struct dm_stats s;

static void print_stats(void)
{
	static const char *const states[] = {"DOWN", "DEGRADED", "UP"};

	dm_stats_get(&s);
	printk("\nclass  submitted  refused  sent  failed  dropped(quota/age/shed)  coalesced"
	       "  airtime_ms\n");
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_stats *k = &s.cls[c];

		printk("C%d     %9u  %7u  %4u  %6u  %6u/%3u/%4u          %9u  %10u\n", c,
		       k->submitted, k->refused, k->sent, k->failed, k->dropped_quota,
		       k->dropped_age, k->dropped_shed, k->coalesced, k->airtime_ms_used);
	}
	printk("\nlink  state     tx_ok  tx_fail  probes  switches_to  score  rtt_us  loss_x10\n");
	for (uint8_t l = 0; l < dm_link_count(); l++) {
		const struct dm_link_stats *k = &s.link[l];

		printk("%u     %-8s  %5u  %7u  %2u/%-3u  %11u  %5u  %6u  %8u\n", l,
		       states[k->state], k->tx_ok, k->tx_fail, k->probes_ok, k->probes_fail,
		       k->switches_to, k->score, k->ewma_rtt_us, k->ewma_loss_pct_x10);
	}
	printk("\npolicy v%u, pressure %u, slab %u%%, scheduler rounds %u\n", s.policy_version,
	       s.pressure_state, s.slab_pct, s.sched_rounds);
}

int main(void)
{
	const struct dm_fake_cfg radio = {
		.caps = {.mtu = 127, .tier = DM_TIER_MAC, .mac_prio_levels = 2,
			 .probe_period_ms = 1000},
		.bitrate_bps = 250000,
		.overhead_us = 800,
		.airtime_latency = true,
		.loss_pct = 5,
		.probe_rtt_us = 8000,
		.seed = 1,
	};
	const struct dm_fake_cfg cell = {
		.caps = {.mtu = 512, .tier = DM_TIER_SW, .metered = true, .probe_period_ms = 2000},
		.bitrate_bps = 50000,
		.overhead_us = 20000,
		.airtime_latency = true,
		.probe_rtt_us = 150000,
		.seed = 2,
	};
	uint32_t c0 = 0;
	uint32_t seq = 0;

	dm_fake_configure(RADIO, &radio);
	dm_fake_configure(CELL, &cell);
	dm_fake_set_tx_cb(on_tx);
	dm_hint_register(on_hint);
	if (dm_fake_register(RADIO, &link_radio) != 0 || dm_fake_register(CELL, &link_cell) != 0) {
		printk("link registration failed\n");
		return 0;
	}
	printk("dockmaster up: radio is link %u, cellular (metered) is link %u\n", link_radio,
	       link_cell);

	for (uint32_t t = 0; t < RUN_MS; t += 10) {
		if (t % 1000 == 0) {
			(void)submit(DM_C0, c0++, 16, 0, NULL);
		}
		if (t % 100 == 0) {
			struct dm_submit_opts o = {.coalesce_key = (t / 100) % 4,
						   .link_hint = DM_LINK_ANY};

			(void)submit(DM_C1, seq++, 24, DM_F_COALESCE, &o);
		}
		if (t % 50 == 0) {
			struct dm_submit_opts o = {.deadline_ms = 300, .link_hint = DM_LINK_ANY};

			(void)submit(DM_C2, seq++, 100, DM_F_DROP_LATE, &o);
		}
		if (t % 500 == 0) {
			(void)submit(DM_C3, seq++, 64, 0, NULL);
		}
		if (t % 20 == 0) {
			(void)submit(DM_C4, seq++, 200, DM_F_RESUMABLE, NULL);
		}
		if (t == 4000) {
			printk("link 0 down (radio fails for 3 s)\n");
			dm_fake_cfg(RADIO)->down = true;
			dm_link_state(link_radio, DM_LINK_DOWN);
		}
		if (t == 7000) {
			printk("link 0 up\n");
			dm_fake_cfg(RADIO)->down = false;
			dm_link_state(link_radio, DM_LINK_UP);
		}
		k_msleep(10);
	}

	k_msleep(5000); /* let the queues drain */
	print_stats();

	uint32_t delivered = 0;

	for (uint32_t i = 0; i < c0; i++) {
		delivered += atomic_test_bit(c0_seen, i) ? 1U : 0U;
	}
	dm_stats_get(&s);
	printk("C0 delivered %u/%u, lost %u\n", delivered, c0, s.c0_lost);
	printk("C4 bytes on metered link: %ld\n", (long)atomic_get(&c4_cell_bytes));
	printk("dockmaster sample done\n");
	return 0;
}
