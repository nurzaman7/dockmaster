/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full stack on two fake links (spec section 15): 60 s at five times the
 * nominal load, the radio flapping every 7 s and the metered link 10 %
 * lossy. The real scheduler and owner threads run; nothing is stepped.
 */

#include "dm_test_common.h"

#define RADIO      0
#define CELL       1
#define RUN_MS     60000
#define FLAP_MS    7000
#define TICK_MS    10
#define C0_MAX     1024
#define C1_KEYS    8

static ATOMIC_DEFINE(c0_seen, C0_MAX);
static atomic_t c4_cell_bytes;
static uint32_t c1_last_sub[C1_KEYS];
static volatile uint32_t c1_last_got[C1_KEYS];

static void on_tx(uint8_t fake, const uint8_t *buf, size_t len, int status)
{
	uint32_t seq = sys_get_le32(&buf[1]);

	if (status != 0) {
		return;
	}
	switch (buf[0]) {
	case DM_C0:
		atomic_set_bit(c0_seen, seq % C0_MAX);
		break;
	case DM_C1:
		/* Completions may arrive out of order across links: keep the newest. */
		if (seq > c1_last_got[buf[5]]) {
			c1_last_got[buf[5]] = seq;
		}
		break;
	case DM_C4:
		if (fake == CELL) {
			atomic_add(&c4_cell_bytes, (atomic_val_t)len);
		}
		break;
	default:
		break;
	}
}

static int submit(dm_class_t c, uint32_t seq, uint8_t key, size_t len, uint32_t flags,
		  uint32_t deadline_ms)
{
	static uint8_t buf[DM_MAX_MSG];
	struct dm_submit_opts o = {.coalesce_key = key, .deadline_ms = deadline_ms,
				   .link_hint = DM_LINK_ANY};

	memset(buf, 0x3C, len);
	buf[0] = (uint8_t)c;
	sys_put_le32(seq, &buf[1]);
	buf[5] = key;
	return dm_submit(c, buf, len, flags, &o);
}

ZTEST_SUITE(integration, NULL, NULL, NULL, NULL, NULL);

ZTEST(integration, test_two_links_flapping_lossy)
{
	struct dm_test_cfg cfg = {.manual_sched = false};
	const struct dm_fake_cfg radio = {
		.caps = {.mtu = 127, .tier = DM_TIER_MAC, .mac_prio_levels = 2,
			 .probe_period_ms = 1000},
		.bitrate_bps = 250000,
		.overhead_us = 800,
		.airtime_latency = true,
		.probe_rtt_us = 10000,
		.seed = 11,
	};
	const struct dm_fake_cfg cell = {
		.caps = {.mtu = 512, .tier = DM_TIER_SW, .metered = true,
			 .probe_period_ms = 2000},
		.bitrate_bps = 50000,
		.overhead_us = 20000,
		.airtime_latency = true,
		.loss_pct = 10,
		.probe_rtt_us = 150000,
		.seed = 12,
	};
	static struct dm_stats s;
	uint32_t c0_sub = 0;
	uint32_t seq = 1;
	bool radio_up = true;

	dm_fake_reset_all();
	dm_test_reset(&cfg);
	dm_fake_set_tx_cb(on_tx);
	dm_fake_configure(RADIO, &radio);
	dm_fake_configure(CELL, &cell);
	zassert_ok(dm_fake_register(RADIO, &(uint8_t){0}));
	zassert_ok(dm_fake_register(CELL, &(uint8_t){0}));

	/*
	 * Budgets are per link, and the cellular link is slow (about 23 ms of air
	 * per small frame), so the policy sizes two of them for it:
	 *  - C0 is paced by its bucket even though it may overdraw once. At 5x
	 *    load it needs 5 x 22.6 ms = 113 ms/s while the radio is down; the
	 *    default 50 ms/s carries two messages a second and Dockmaster would
	 *    correctly refuse the rest.
	 *  - C1 state must flush every key inside max_age (5 s): 8 keys x 23.8 ms
	 *    at the default 30 ms/s takes 6.3 s, so the newest values would age
	 *    out. C1 stays on the cellular link here because the radio flaps.
	 */
	struct dm_policy p;

	dm_policy_default(&p);
	p.version = 2;
	p.cls[DM_C0].bucket[CELL] = (struct dm_bucket){.rate_ms_per_s = 200, .burst_ms = 500};
	p.cls[DM_C1].bucket[CELL] = (struct dm_bucket){.rate_ms_per_s = 100, .burst_ms = 300};
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));

	/* Nominal per second: C0 1, C1 10, C2 20, C3 2, C4 10. Five times that. */
	for (uint32_t t = 0; t < RUN_MS; t += TICK_MS) {
		if (t % 200 == 0) {
			int rc = submit(DM_C0, c0_sub, 0, 16, 0, 0);

			zassert_ok(rc, "C0 refused at %u ms: rc %d, C0 queued %u", t, rc,
				   dm_queue_count(DM_C0));
			c0_sub++;
		}
		if (t % 20 == 0) {
			uint8_t key = (uint8_t)((t / 20) % C1_KEYS);

			if (submit(DM_C1, seq, key, 24, DM_F_COALESCE, 0) == 0) {
				c1_last_sub[key] = seq;
			}
			seq++;
		}
		if (t % 10 == 0) {
			(void)submit(DM_C2, seq++, 0, 100, DM_F_DROP_LATE, 300);
		}
		if (t % 100 == 0) {
			(void)submit(DM_C3, seq++, 0, 64, 0, 0);
		}
		if (t % 20 == 0) {
			(void)submit(DM_C4, seq++, 0, 200, DM_F_RESUMABLE, 0);
		}
		if (t != 0 && t % FLAP_MS == 0) {
			radio_up = !radio_up;
			dm_fake_cfg(RADIO)->down = !radio_up;
			dm_link_state(RADIO, radio_up ? DM_LINK_UP : DM_LINK_DOWN);
		}
		k_msleep(TICK_MS);
	}

	/* Stop the load, bring the radio back and let everything drain. */
	dm_fake_cfg(RADIO)->down = false;
	dm_link_state(RADIO, DM_LINK_UP);
	for (int i = 0; i < 200 && dm_slab_used() != 0U; i++) {
		k_msleep(100);
	}
	dm_stats_get(&s);

	TC_PRINT("slab %u%%, c0_lost %u, sched rounds %u, max round %u us\n", s.slab_pct,
		 s.c0_lost, s.sched_rounds, s.sched_max_us);
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_stats *k = &s.cls[c];

		TC_PRINT("C%d submitted %u refused %u sent %u failed %u dropped q/a/s %u/%u/%u "
			 "coalesced %u\n",
			 c, k->submitted, k->refused, k->sent, k->failed, k->dropped_quota,
			 k->dropped_age, k->dropped_shed, k->coalesced);
	}
	for (int l = 0; l < 2; l++) {
		TC_PRINT("L%d ok %u fail %u faults %u probes %u/%u switches_to %u\n", l,
			 s.link[l].tx_ok, s.link[l].tx_fail, s.link[l].faults, s.link[l].probes_ok,
			 s.link[l].probes_fail, s.link[l].switches_to);
	}

	/* C0: every message delivered, none lost. */
	zassert_equal(s.c0_lost, 0);
	zassert_equal(s.cls[DM_C0].submitted, c0_sub);
	for (uint32_t i = 0; i < c0_sub; i++) {
		zassert_true(atomic_test_bit(c0_seen, i), "C0 #%u never delivered", i);
	}

	/* C1: the freshest value of every key made it. */
	for (int k = 0; k < C1_KEYS; k++) {
		zassert_equal(c1_last_got[k], c1_last_sub[k], "key %d: got %u, newest was %u", k,
			      c1_last_got[k], c1_last_sub[k]);
	}

	/* C4: not one byte over the metered link. */
	zassert_equal(atomic_get(&c4_cell_bytes), 0);

	/* No leak, and every accepted message is accounted for. */
	zassert_equal(dm_slab_used(), 0, "slab leak: %u blocks", dm_slab_used());
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_stats *k = &s.cls[c];
		uint32_t out = k->sent + k->failed + k->dropped_quota + k->dropped_age +
			       k->dropped_shed + k->coalesced;

		zassert_equal(k->submitted, out, "C%d: %u accepted, %u accounted for", c,
			      k->submitted, out);
		zassert_equal(k->bytes_queued, 0);
	}
	zassert_true(s.link[RADIO].switches_to + s.link[CELL].switches_to >= 4,
		     "the flapping radio should cause failovers and returns");

	if (!IS_ENABLED(CONFIG_ARCH_POSIX) && !IS_ENABLED(CONFIG_QEMU_TARGET)) {
		zassert_true(s.sched_max_us < 50, "scheduler round took %u us", s.sched_max_us);
	}
}
