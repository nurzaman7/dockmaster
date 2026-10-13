/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Scheduler (spec section 8). Fake clock, exact token values. The fake link
 * runs at 1 Mbit/s with no overhead, so a message costs 8 us per byte.
 */

#include "dm_test_common.h"

#define US_PER_BYTE 8

static void before(void *f)
{
	ARG_UNUSED(f);
	dmt_reset();
}

ZTEST_SUITE(sched, NULL, NULL, before, NULL, NULL);

/* Default policy without C0 duplication, so one link is enough. */
static void policy_single(struct dm_policy *p)
{
	dmt_policy(p, 2);
	p->cls[DM_C0].flags = 0;
}

static int32_t tokens(dm_class_t c, uint8_t l)
{
	return dm_sched_bucket(c, l)->tokens_us;
}

ZTEST(sched, test_strict_priority_all_wet)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_policy p;

	(void)dmt_add_link(0, &cfg);
	policy_single(&p);
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));

	for (int c = DM_C4; c >= DM_C0; c--) {
		struct dm_submit_opts o = {.coalesce_key = 1, .link_hint = DM_LINK_ANY};

		zassert_ok(dmt_submit((dm_class_t)c, (uint32_t)c, 20, 0, &o));
	}
	dmt_drain(20);
	zassert_equal(dmt_log_n, DM_CLASS_COUNT);
	for (int i = 0; i < DM_CLASS_COUNT; i++) {
		zassert_equal(dmt_log[i].cls, i, "position %d carried C%u", i, dmt_log[i].cls);
	}
	zassert_equal(dm_slab_used(), 0);
}

ZTEST(sched, test_dry_c3_falls_through_to_c4)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_policy p;

	(void)dmt_add_link(0, &cfg);
	dmt_policy(&p, 2);
	p.cls[DM_C3].bucket[0] = (struct dm_bucket){.rate_ms_per_s = 1, .burst_ms = 1};
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	dm_sched_link_added(0, dm_now()); /* buckets start full at the new burst */

	zassert_ok(dmt_submit(DM_C3, 1, 200, 0, NULL)); /* 1600 us > 1000 us */
	zassert_ok(dmt_submit(DM_C4, 2, 20, 0, NULL));
	dmt_round();
	dmt_round();
	zassert_equal(dmt_log_n, 1);
	zassert_equal(dmt_log[0].cls, DM_C4, "dry C3 must fall through to C4");
	zassert_equal(dm_queue_count(DM_C3), 1, "C3 waits for its bucket");
}

ZTEST(sched, test_c0_overdraws)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_policy p;
	const int32_t cost = 240 * US_PER_BYTE;

	(void)dmt_add_link(0, &cfg);
	policy_single(&p);
	p.cls[DM_C0].bucket[0] = (struct dm_bucket){.rate_ms_per_s = 100, .burst_ms = 1};
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	dm_sched_link_added(0, dm_now());
	zassert_equal(tokens(DM_C0, 0), 1000);

	zassert_ok(dmt_submit(DM_C0, 1, 240, 0, NULL));
	zassert_ok(dmt_submit(DM_C0, 2, 240, 0, NULL));
	dmt_round(); /* first goes out although 1000 us < 1920 us */
	dmt_round();
	zassert_equal(dmt_log_n, 1);
	zassert_equal(tokens(DM_C0, 0), 1000 - cost, "C0 overdrew to a negative balance");

	dm_test_clock_advance_ms(9); /* +900 us: still negative */
	dmt_round();
	zassert_equal(dmt_log_n, 1, "C0 must recover before sending again");
	zassert_equal(tokens(DM_C0, 0), 1000 - cost + 900);

	dm_test_clock_advance_ms(1); /* +100 us: back to >= 0 */
	dmt_round();
	dmt_round();
	zassert_equal(dmt_log_n, 2);
	zassert_equal(tokens(DM_C0, 0), 1000 - cost + 1000 - cost);
}

ZTEST(sched, test_measured_airtime_debits_not_estimate)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	const int32_t burst = 1000 * 1000; /* default C3 burst: 1000 ms */

	cfg.airtime_pct = 50; /* the radio reports half the estimate */
	(void)dmt_add_link(0, &cfg);
	zassert_equal(tokens(DM_C3, 0), burst);

	zassert_ok(dmt_submit(DM_C3, 1, 100, 0, NULL));
	dmt_round();
	zassert_equal(tokens(DM_C3, 0), burst - 800, "estimate reserved at dispatch");
	dmt_round();
	zassert_equal(tokens(DM_C3, 0), burst - 400, "measured airtime charged on completion");
}

ZTEST(sched, test_refill_across_tick_wrap)
{
	struct dm_bucket p = {.rate_ms_per_s = 333, .burst_ms = 1000};
	struct dm_bucket_state one = {.tokens_us = 0, .last_refill_tick = 0xFFFFFF00U};
	struct dm_bucket_state many = one;
	uint64_t us = k_ticks_to_us_floor64(0x200);

	dm_bucket_refill(&one, &p, 0x00000100U);
	zassert_equal(one.tokens_us, (int32_t)(us * 333U / 1000U), "dt = 0x200 ticks across wrap");
	zassert_equal(one.last_refill_tick, 0x100U);

	/* Many small refills lose nothing: the remainder is carried. */
	for (uint32_t t = 0xFFFFFF01U; t != 0x101U; t++) {
		dm_bucket_refill(&many, &p, t);
	}
	zassert_equal(many.tokens_us, one.tokens_us);

	/* Long idle: capped at the burst, no overflow. */
	dm_bucket_refill(&one, &p, 0x7FFFFFFFU);
	zassert_equal(one.tokens_us, 1000 * 1000);
}

ZTEST(sched, test_half_duplex_charge)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	const int32_t burst = 1000 * 1000;

	cfg.caps.half_duplex = true;
	cfg.rx_turnaround_us = 500;
	(void)dmt_add_link(0, &cfg);
	zassert_ok(dmt_submit(DM_C3, 1, 100, 0, NULL));
	dmt_round();
	dmt_round();
	zassert_equal(tokens(DM_C3, 0), burst - 800 - 500, "rx-off time is charged too");
}

ZTEST(sched, test_dup_on_two_links)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_stats s;

	(void)dmt_add_link(0, &cfg);
	(void)dmt_add_link(1, &cfg);
	dmt_round();
	zassert_ok(dmt_submit(DM_C0, 1, 16, 0, NULL));
	dmt_round(); /* both copies dispatched in one round */
	zassert_equal(dmt_count(0, DM_C0), 1);
	zassert_equal(dmt_count(1, DM_C0), 1);
	zassert_equal(dm_slab_used(), 1, "block held until the second completion");
	dmt_round();
	zassert_equal(dm_slab_used(), 0);
	dm_stats_get(&s);
	zassert_equal(s.cls[DM_C0].sent, 1, "one message, delivered twice");
}

ZTEST(sched, test_dup_pending_until_second_link)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);

	(void)dmt_add_link(0, &cfg);
	dmt_round();
	zassert_ok(dmt_submit(DM_C0, 1, 16, 0, NULL));
	dmt_round();
	dmt_round();
	zassert_equal(dm_slab_used(), 1, "copy still pending");

	(void)dmt_add_link(1, &cfg);
	dmt_round(); /* eligibility now has two links */
	dmt_round();
	dmt_round();
	zassert_equal(dmt_count(1, DM_C0), 1, "second copy sent when a link appeared");
	zassert_equal(dm_slab_used(), 0);
}

ZTEST(sched, test_dup_window_expires)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);

	(void)dmt_add_link(0, &cfg);
	dmt_round();
	zassert_ok(dmt_submit(DM_C0, 1, 16, 0, NULL));
	dmt_round();
	dmt_round();
	zassert_equal(dm_slab_used(), 1);
	dm_test_clock_advance_ms(CONFIG_DM_DUP_WINDOW_MS);
	dmt_round();
	zassert_equal(dm_slab_used(), 1, "window not over yet");
	dm_test_clock_advance_ms(1);
	dmt_round();
	zassert_equal(dm_slab_used(), 0, "second copy abandoned after the window");
}

ZTEST(sched, test_tx_never_overlaps)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_fake_counters fc;

	cfg.latency_us = 1000; /* below the 4 x 400 us watchdog */
	(void)dmt_add_link(0, &cfg);
	for (uint32_t i = 0; i < 8; i++) {
		zassert_ok(dmt_submit(DM_C3, i, 50, 0, NULL));
	}
	for (int i = 0; i < 40 && dm_slab_used() != 0U; i++) {
		dmt_round();
		k_msleep(1);
	}
	dm_fake_counters(0, &fc);
	zassert_equal(fc.tx_calls, 8);
	zassert_equal(fc.overlap, 0, "tx called while a previous tx was outstanding");
	zassert_equal(dm_slab_used(), 0);
}

ZTEST(sched, test_watchdog_fault)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_fake_counters fc;
	struct dm_stats s;

	cfg.drop_completion = true;
	(void)dmt_add_link(0, &cfg);
	zassert_ok(dmt_submit(DM_C3, 1, 100, 0, NULL)); /* watchdog 4 x 800 us */
	dmt_round();
	k_msleep(5);
	dmt_round();

	dm_fake_counters(0, &fc);
	dm_stats_get(&s);
	zassert_equal(fc.cancels, 1, "missing completion is cancelled");
	zassert_equal(s.link[0].faults, 1);
	zassert_equal(s.cls[DM_C3].failed, 1);
	zassert_equal(dm_slab_used(), 0);

	dm_link_tx_done(0, 1, 0, 0); /* too late: ignored */
	zassert_equal(atomic_get(&dm_ctr.late_done), 1);
}

ZTEST(sched, test_c0_retried_after_failure)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_policy p;
	struct dm_stats s;

	cfg.loss_pct = 100;
	(void)dmt_add_link(0, &cfg);
	policy_single(&p);
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));

	zassert_ok(dmt_submit(DM_C0, 1, 16, 0, NULL));
	dmt_round();
	dm_fake_cfg(0)->loss_pct = 0;
	dmt_drain(10);
	dm_stats_get(&s);
	zassert_equal(dmt_log_n, 2);
	zassert_equal(dmt_log[0].status, -EIO);
	zassert_equal(dmt_log[1].status, 0);
	zassert_equal(s.cls[DM_C0].sent, 1, "control is retried, never dropped");
	zassert_equal(s.cls[DM_C0].failed, 0);
}

ZTEST(sched, test_mac_priority_mapping)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_fake_counters fc;
	struct dm_policy p;

	(void)dmt_add_link(0, &cfg);
	policy_single(&p);
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	zassert_ok(dmt_submit(DM_C0, 1, 16, 0, NULL));
	dmt_drain(5);
	dm_fake_counters(0, &fc);
	zassert_equal(fc.last_mac_prio, 0);
	zassert_ok(dmt_submit(DM_C4, 2, 16, 0, NULL));
	dmt_drain(5);
	dm_fake_counters(0, &fc);
	zassert_equal(fc.last_mac_prio, 1, "2 levels: C3 and C4 use the low slot");
}

ZTEST(sched, test_sw_edca_waits_and_gives_up)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_fake_counters fc;
	struct dm_stats s;

	cfg.caps.tier = DM_TIER_SW;
	cfg.busy_checks = 2; /* medium busy twice, then free */
	(void)dmt_add_link(0, &cfg);
	zassert_ok(dmt_submit(DM_C3, 1, 16, 0, NULL));
	dmt_round();
	k_msleep(2000);
	dmt_round();
	dm_fake_counters(0, &fc);
	dm_stats_get(&s);
	zassert_equal(fc.busy_checks, 3);
	zassert_equal(fc.tx_calls, 1);
	zassert_true(s.link[0].busy_wait_ms >= 3U * CONFIG_DM_EDCA_AIFS_US_C3 / 1000U);

	dm_fake_cfg(0)->busy_checks = 100; /* never free: give up after the retries */
	zassert_ok(dmt_submit(DM_C3, 2, 16, 0, NULL));
	dmt_round();
	k_msleep(10000);
	dmt_round();
	dm_fake_counters(0, &fc);
	dm_stats_get(&s);
	zassert_equal(fc.tx_calls, 1, "no transmission on a busy medium");
	zassert_equal(s.link[0].tx_fail, 1);
	zassert_equal(s.cls[DM_C3].failed, 1);
}
