/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backpressure (spec section 12), including the 10,000 iteration property
 * test: whatever the load, C0 is never lost while one link is up.
 */

#include "dm_test_common.h"

static const uint8_t thr[2] = {60, 85};

struct hint_rec {
	dm_class_t cls;
	enum dm_pressure p;
};
static struct hint_rec hints[16];
static int n_hints;

static void record_hint(dm_class_t cls, enum dm_pressure p)
{
	if (n_hints < ARRAY_SIZE(hints)) {
		hints[n_hints] = (struct hint_rec){cls, p};
	}
	n_hints++;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	dmt_reset();
	dm_pressure_test_reset();
	n_hints = 0;
}

ZTEST_SUITE(pressure, NULL, NULL, before, NULL, NULL);

ZTEST(pressure, test_transitions_and_hysteresis)
{
	zassert_equal(dm_pressure_next(DM_PRESSURE_NORMAL, 59, thr), DM_PRESSURE_NORMAL);
	zassert_equal(dm_pressure_next(DM_PRESSURE_NORMAL, 60, thr), DM_PRESSURE_TIGHT);
	zassert_equal(dm_pressure_next(DM_PRESSURE_NORMAL, 90, thr), DM_PRESSURE_SHED);
	zassert_equal(dm_pressure_next(DM_PRESSURE_TIGHT, 50, thr), DM_PRESSURE_TIGHT);
	zassert_equal(dm_pressure_next(DM_PRESSURE_TIGHT, 49, thr), DM_PRESSURE_NORMAL);
	zassert_equal(dm_pressure_next(DM_PRESSURE_TIGHT, 85, thr), DM_PRESSURE_SHED);
	zassert_equal(dm_pressure_next(DM_PRESSURE_SHED, 75, thr), DM_PRESSURE_SHED);
	zassert_equal(dm_pressure_next(DM_PRESSURE_SHED, 74, thr), DM_PRESSURE_TIGHT);
	zassert_equal(dm_pressure_next(DM_PRESSURE_SHED, 30, thr), DM_PRESSURE_NORMAL);
	zassert_equal(dm_pressure_next(DM_PRESSURE_CRITICAL, 0, thr), DM_PRESSURE_CRITICAL);
}

ZTEST(pressure, test_boundary_class)
{
	zassert_equal(dm_pressure_boundary(DM_PRESSURE_NORMAL, DM_PRESSURE_TIGHT), DM_C2);
	zassert_equal(dm_pressure_boundary(DM_PRESSURE_TIGHT, DM_PRESSURE_NORMAL), DM_C2);
	zassert_equal(dm_pressure_boundary(DM_PRESSURE_TIGHT, DM_PRESSURE_SHED), DM_C3);
	zassert_equal(dm_pressure_boundary(DM_PRESSURE_SHED, DM_PRESSURE_TIGHT), DM_C3);
	zassert_equal(dm_pressure_boundary(DM_PRESSURE_NORMAL, DM_PRESSURE_SHED), DM_C3);
	zassert_equal(dm_pressure_boundary(DM_PRESSURE_SHED, DM_PRESSURE_CRITICAL), DM_C0);
}

static void fill_to(uint32_t blocks)
{
	static uint32_t seq;

	while (dm_slab_used() < blocks) {
		dm_class_t c = dm_queue_count(DM_C3) < CONFIG_DM_Q_DEPTH_C3 ? DM_C3 : DM_C2;

		zassert_ok(dmt_submit(c, seq++, 8, 0, NULL));
	}
}

static void shed_to(uint32_t blocks)
{
	uint32_t used = dm_slab_used();

	zassert_true(dm_q_consumer_trylock());
	(void)dm_shed(0, used - blocks, DM_C1);
	dm_q_consumer_unlock();
	zassert_equal(dm_slab_used(), blocks);
}

ZTEST(pressure, test_refusals_and_hints)
{
	dm_hint_register(record_hint);

	fill_to(20); /* 62 % */
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_TIGHT);
	zassert_equal(dmt_submit(DM_C4, 0, 8, 0, NULL), -ENOSPC, "TIGHT refuses C4");
	zassert_ok(dmt_submit(DM_C3, 0, 8, 0, NULL));

	fill_to(28); /* 87 % */
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_SHED);
	zassert_equal(dmt_submit(DM_C3, 0, 8, 0, NULL), -ENOSPC, "SHED refuses C3");
	zassert_ok(dmt_submit(DM_C2, 0, 8, 0, NULL));
	zassert_true(dm_slab_used() < 28U, "SHED sheds proactively each round");
	dmt_round();
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_SHED, "75 %% hysteresis holds");

	shed_to(22); /* 68 % */
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_TIGHT);
	shed_to(15); /* 46 % */
	dmt_round();
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_NORMAL);
	zassert_ok(dmt_submit(DM_C4, 0, 8, 0, NULL));

	zassert_equal(n_hints, 4, "exactly one hint per transition, got %d", n_hints);
	zassert_equal(hints[0].cls, DM_C2);
	zassert_equal(hints[0].p, DM_PRESSURE_TIGHT);
	zassert_equal(hints[1].cls, DM_C3);
	zassert_equal(hints[1].p, DM_PRESSURE_SHED);
	zassert_equal(hints[2].cls, DM_C3);
	zassert_equal(hints[2].p, DM_PRESSURE_TIGHT);
	zassert_equal(hints[3].cls, DM_C2);
	zassert_equal(hints[3].p, DM_PRESSURE_NORMAL);
}

ZTEST(pressure, test_forced_coalesce_in_shed)
{
	fill_to(28);
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_SHED);
	zassert_ok(dmt_submit(DM_C1, 1, 8, 0, NULL)); /* no flag, key 0 */
	zassert_ok(dmt_submit(DM_C1, 2, 8, 0, NULL));
	zassert_equal(dm_queue_count(DM_C1), 1, "C1 coalesce is forced on in SHED");
}

ZTEST(pressure, test_critical_is_sticky)
{
	struct dm_policy p;

	dm_hint_register(record_hint);
	dmt_policy(&p, 2);
	p.cls[DM_C0].quota_bytes = 10;
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	zassert_ok(dmt_submit(DM_C0, 1, 10, 0, NULL));
	zassert_equal(dmt_submit(DM_C0, 2, 10, 0, NULL), -ENOSPC);
	dmt_round();
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_CRITICAL);
	zassert_equal(n_hints, 1);
	zassert_equal(hints[0].cls, DM_C0);
	dm_stats_reset();
	dmt_round();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_NORMAL, "left only by an explicit reset");
}

static uint32_t rng = 0x1234567U;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

ZTEST(pressure, test_property_c0_never_lost)
{
	struct dm_fake_cfg slow = dmt_link_cfg(false);
	struct dm_fake_cfg flaky = dmt_link_cfg(true);
	enum dm_pressure worst = DM_PRESSURE_NORMAL;
	uint32_t c0_ok = 0;
	struct dm_stats s;

	slow.bitrate_bps = 100000; /* 240 B take 19 ms: a backlog builds */
	flaky.bitrate_bps = 20000;
	flaky.loss_pct = 30;
	flaky.seed = 7;
	(void)dmt_add_link(0, &slow);
	(void)dmt_add_link(1, &flaky);
	dmt_round();

	for (int i = 0; i < 10000; i++) {
		uint32_t r = rnd() % 100U;
		size_t len = 8U + rnd() % (DM_MAX_MSG - 8U);
		struct dm_submit_opts o = {.coalesce_key = rnd() % 8U, .deadline_ms = 200,
					   .link_hint = DM_LINK_ANY};
		dm_class_t c = r < 2 ? DM_C0 : r < 20 ? DM_C1 : r < 45 ? DM_C2 : r < 70 ? DM_C3
											  : DM_C4;
		int rc = dmt_submit(c, (uint32_t)i, c == DM_C0 ? 16 : len,
				    c == DM_C2 ? DM_F_DROP_LATE : 0, &o);

		if (c == DM_C0) {
			zassert_ok(rc, "C0 refused at iteration %d (%d)", i, rc);
			c0_ok++;
		}
		/* The second link flaps; the first stays up. */
		if (rnd() % 500U == 0U) {
			dm_link_state(1, (rnd() & 1U) ? DM_LINK_UP : DM_LINK_DOWN);
		}
		dm_test_clock_advance_ms(1 + rnd() % 5U);
		dmt_round();
		worst = MAX(worst, dm_pressure_get());
	}
	dm_link_state(1, DM_LINK_UP);
	for (int i = 0; i < 2000 && dm_queue_count(DM_C0) != 0U; i++) {
		dm_test_clock_advance_ms(10);
		dmt_round();
	}
	dm_test_clock_advance_ms(CONFIG_DM_DUP_WINDOW_MS + 1);
	dmt_round();
	dmt_round();

	dm_stats_get(&s);
	TC_PRINT("worst pressure %d, C0 %u sent, C4 refused %u, C3 refused %u\n", worst,
		 s.cls[DM_C0].sent, s.cls[DM_C4].refused, s.cls[DM_C3].refused);
	zassert_true(worst >= DM_PRESSURE_TIGHT, "the load never built up any pressure");
	zassert_equal(s.c0_lost, 0);
	zassert_equal(s.cls[DM_C0].submitted, c0_ok);
	zassert_equal(s.cls[DM_C0].sent, c0_ok, "every C0 message delivered");
}
