/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Link manager (spec section 10): pure scoring functions, then scripted
 * sequences of probe results and state changes on two fake links.
 */

#include "dm_test_common.h"

#define A 0
#define B 1

static void before(void *f)
{
	ARG_UNUSED(f);
	dmt_reset();
}

ZTEST_SUITE(linkmgr, NULL, NULL, before, NULL, NULL);

static void two_links(bool b_metered)
{
	struct dm_fake_cfg a = dmt_link_cfg(false);
	struct dm_fake_cfg b = dmt_link_cfg(b_metered);

	zassert_equal(dmt_add_link(A, &a), A);
	zassert_equal(dmt_add_link(B, &b), B);
	dmt_round();
}

/* Ask for one probe on @p link and settle its result. */
static void probe_cycle(uint8_t link)
{
	atomic_set(&dm_links[link].probe_due, 1);
	dmt_round(); /* dispatches the probe; the owner starts it */
	k_msleep(1); /* the fake answers from a timer */
	dmt_round(); /* settles the result */
}

ZTEST(linkmgr, test_ewma_update)
{
	uint32_t acc = 1000U << 3;

	dm_ewma_q3(&acc, 1000);
	zassert_equal(acc >> 3, 1000, "steady input keeps the value");
	dm_ewma_q3(&acc, 2000);
	zassert_equal(acc >> 3, 1125, "gain is 1/8");
	for (int i = 0; i < 100; i++) {
		dm_ewma_q3(&acc, 2000);
	}
	zassert_equal(acc >> 3, 2000, "Q3 accumulator converges exactly");
}

ZTEST(linkmgr, test_score_monotonic)
{
	struct dm_lm_inputs in = {
		.tput_bps = 250000, .rtt_us = 20000, .state = DM_LINK_UP,
	};
	uint16_t prev = UINT16_MAX;

	for (uint16_t loss = 0; loss <= 1000; loss += 10) {
		in.loss_x10 = loss;
		uint16_t s = dm_lm_score_calc(&in);

		zassert_true(s <= prev, "score rose with loss at %u", loss);
		zassert_true(s <= 1000);
		prev = s;
	}
	in.loss_x10 = 0;
	uint16_t up = dm_lm_score_calc(&in);

	in.loss_x10 = 1000;
	zassert_true(dm_lm_score_calc(&in) + 200 < up, "full loss costs W_LOSS points");
	in.loss_x10 = 0;

	in.metered = true;
	zassert_true(dm_lm_score_calc(&in) < up, "metered costs points");
	in.metered = false;
	in.state = DM_LINK_DEGRADED;
	zassert_true(dm_lm_score_calc(&in) < up);
	in.state = DM_LINK_DOWN;
	zassert_equal(dm_lm_score_calc(&in), 0, "DOWN scores 0");
	in.state = DM_LINK_UP;
	in.rtt_us = 2000000;
	zassert_true(dm_lm_score_calc(&in) < up, "longer RTT costs points");
	in.rtt_us = 20000;
	in.tput_bps = 5000;
	zassert_true(dm_lm_score_calc(&in) < up, "lower throughput costs points");
}

ZTEST(linkmgr, test_switch_rule)
{
	/* CONFIG_DM_SWITCH_MARGIN_PCT = 20: the bar over 500 is 600. */
	zassert_false(dm_lm_switch_ok(600, 500, 10000, 2000), "equal to the bar is not enough");
	zassert_true(dm_lm_switch_ok(601, 500, 10000, 2000));
	zassert_false(dm_lm_switch_ok(900, 500, 1999, 2000), "dwell not over");
	zassert_true(dm_lm_switch_ok(900, 500, 2000, 2000));
	zassert_true(dm_lm_switch_ok(1, 0, 0, dm_lm_dwell_ms(DM_C0)), "C0 has no dwell");
	zassert_equal(dm_lm_dwell_ms(DM_C4), 300000);
}

ZTEST(linkmgr, test_flap_penalty_decay)
{
	uint32_t hl = CONFIG_DM_FLAP_HALFLIFE_S * 1000U;

	zassert_equal(dm_lm_flap_decay(0, hl), 1000);
	zassert_equal(dm_lm_flap_decay(hl - 1, hl), 1000);
	zassert_equal(dm_lm_flap_decay(hl, hl), 500);
	zassert_equal(dm_lm_flap_decay(2 * hl, hl), 250);
	zassert_equal(dm_lm_flap_decay(10 * hl, hl), 0);
}

ZTEST(linkmgr, test_down_link_excluded_until_probe)
{
	two_links(false);
	zassert_equal(dm_lm_eligible(DM_C3), BIT(A) | BIT(B));
	zassert_equal(dm_lm_current(DM_C3), A);

	dm_link_state(A, DM_LINK_DOWN);
	dmt_round();
	zassert_equal(dm_lm_eligible(DM_C3), BIT(B), "DOWN link removed");
	zassert_equal(dm_lm_current(DM_C3), B, "failover");
	zassert_equal(dm_lm_score(A), 0);

	dm_link_state(A, DM_LINK_UP);
	dmt_round();
	zassert_equal(dm_lm_eligible(DM_C3), BIT(B), "UP is not enough: needs a probe");
	k_msleep(1);
	dmt_round();
	zassert_equal(dm_lm_eligible(DM_C3), BIT(A) | BIT(B), "back after one passed probe");
}

ZTEST(linkmgr, test_metered_excluded_for_exclusive)
{
	two_links(true);
	zassert_equal(dm_lm_eligible(DM_C4), BIT(A), "C4 never on a metered link");
	zassert_equal(dm_lm_eligible(DM_C3), BIT(A) | BIT(B));

	dm_link_state(A, DM_LINK_DOWN);
	dmt_round();
	zassert_equal(dm_lm_eligible(DM_C4), 0, "even with nothing else up");
	zassert_ok(dmt_submit(DM_C4, 1, 40, 0, NULL));
	dmt_round();
	dmt_round();
	zassert_equal(dmt_count(B, DM_C4), 0);
	zassert_equal(dm_queue_count(DM_C4), 1, "bulk waits");
}

ZTEST(linkmgr, test_probe_failures_degrade_then_down)
{
	two_links(false);
	dm_fake_cfg(A)->probe_fail = true;
	for (int i = 0; i < CONFIG_DM_PROBE_FAIL_N - 1; i++) {
		probe_cycle(A);
		zassert_equal(dm_lm_state(A), DM_LINK_UP);
	}
	probe_cycle(A);
	zassert_equal(dm_lm_state(A), DM_LINK_DEGRADED, "N failed probes degrade");
	zassert_true(dm_lm_dup_forced(), "C0 duplication forced while a link is degraded");
	zassert_true(dm_lm_dup_class(DM_C0));

	probe_cycle(A);
	zassert_equal(dm_lm_state(A), DM_LINK_DOWN, "one more failure takes it down");

	dm_fake_cfg(A)->probe_fail = false;
	probe_cycle(A);
	zassert_equal(dm_lm_state(A), DM_LINK_UP, "a passed probe recovers it");
}

ZTEST(linkmgr, test_switch_after_margin_and_dwell)
{
	struct dm_stats s;

	two_links(false);
	zassert_equal(dm_lm_current(DM_C0), A);
	zassert_equal(dm_lm_current(DM_C1), A);

	/* Degrade A: three failed probes raise its loss and mark it DEGRADED. */
	dm_fake_cfg(A)->probe_fail = true;
	for (int i = 0; i < CONFIG_DM_PROBE_FAIL_N; i++) {
		probe_cycle(A);
	}
	zassert_true(dm_lm_score(B) > dm_lm_score(A) + dm_lm_score(A) / 5U,
		     "B now beats A by more than the margin (%u vs %u)", dm_lm_score(B),
		     dm_lm_score(A));

	/* C0 has no dwell: make-before-break probes B, then switches. */
	k_msleep(1);
	dmt_round();
	zassert_equal(dm_lm_current(DM_C0), B, "C0 follows the best link at once");
	zassert_equal(dm_lm_current(DM_C1), A, "C1 still inside its 2 s dwell");

	dm_test_clock_advance_ms(dm_lm_dwell_ms(DM_C1));
	dmt_round();
	k_msleep(1);
	dmt_round();
	zassert_equal(dm_lm_current(DM_C1), B, "C1 switches after its dwell");
	zassert_equal(dm_lm_current(DM_C4), A, "C4 dwell is 300 s");

	dm_stats_get(&s);
	zassert_equal(s.link[B].switches_to, 2);
}

ZTEST(linkmgr, test_failed_make_before_break_probe_cancels)
{
	struct dm_stats s;

	two_links(false);
	dm_fake_cfg(A)->probe_fail = true;
	dm_fake_cfg(B)->probe_fail = true; /* the candidate fails its probe */
	for (int i = 0; i < CONFIG_DM_PROBE_FAIL_N; i++) {
		probe_cycle(A);
	}
	k_msleep(1);
	dmt_round();
	zassert_equal(dm_lm_current(DM_C0), A, "switch cancelled by the failed probe");
	dm_stats_get(&s);
	zassert_equal(s.link[B].switches_to, 0);
	zassert_true(s.link[B].probes_fail >= 1);
}
