/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Queue rules (spec section 7). No scheduler rounds: the tests drain and
 * shed the class queues directly.
 */

#include <zephyr/irq_offload.h>

#include "dm_test_common.h"

static void before(void *f)
{
	ARG_UNUSED(f);
	dmt_reset();
}

ZTEST_SUITE(queue, NULL, NULL, before, NULL, NULL);

static uint32_t head_seq(dm_class_t c)
{
	struct dm_msg_ref ref;

	zassert_true(dm_queue_head(c, &ref), "C%d empty", c);
	return sys_get_le32(&dm_blk_payload(ref.slab_idx)[1]);
}

ZTEST(queue, test_invalid_args)
{
	uint8_t b[4] = {0};
	struct dm_submit_opts bad_hint = {.link_hint = 3};

	zassert_equal(dm_submit(DM_C3, b, 0, 0, NULL), -EINVAL);
	zassert_equal(dm_submit(DM_C3, b, DM_MAX_MSG + 1, 0, NULL), -EINVAL);
	zassert_equal(dm_submit(DM_CLASS_COUNT, b, sizeof(b), 0, NULL), -EINVAL);
	zassert_equal(dm_submit(DM_C3, NULL, sizeof(b), 0, NULL), -EINVAL);
	zassert_equal(dm_submit(DM_C3, b, sizeof(b), 0, &bad_hint), -EINVAL);
	zassert_equal(dm_slab_used(), 0);
}

/* Rule 1 and the O(1) claim: cost does not grow with queue depth. */
ZTEST(queue, test_submit_o1)
{
	uint32_t t_empty;
	uint32_t t_deep;
	uint32_t t0;

	t0 = k_cycle_get_32();
	zassert_ok(dmt_submit(DM_C3, 0, 32, 0, NULL));
	t_empty = k_cycle_get_32() - t0;
	for (uint32_t i = 1; i < CONFIG_DM_Q_DEPTH_C3 - 1; i++) {
		zassert_ok(dmt_submit(DM_C3, i, 32, 0, NULL));
	}
	t0 = k_cycle_get_32();
	zassert_ok(dmt_submit(DM_C3, 99, 32, 0, NULL));
	t_deep = k_cycle_get_32() - t0;
	zassert_equal(dm_queue_count(DM_C3), CONFIG_DM_Q_DEPTH_C3);

	if (IS_ENABLED(CONFIG_ARCH_POSIX)) {
		/* native_sim's cycle counter is simulated time: it does not move
		 * while code runs. The bound is asserted on the Cortex-M target.
		 */
		TC_PRINT("cycle bound not meaningful on native_sim\n");
		return;
	}
	TC_PRINT("dm_submit: %u cycles at depth 0, %u at depth %u (%u us)\n", t_empty, t_deep,
		 CONFIG_DM_Q_DEPTH_C3 - 1, k_cyc_to_us_ceil32(t_deep));
	zassert_true(t_deep <= t_empty + t_empty / 2U + 64U, "submit cost grows with depth");
}

/* Rule 2: quota per class, shed inside the class, never across. */
ZTEST(queue, test_quota_per_class)
{
	struct dm_policy p;
	struct dm_stats s;

	dmt_policy(&p, 2);
	p.cls[DM_C3].quota_bytes = 300;
	p.cls[DM_C0].quota_bytes = 100;
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));

	zassert_ok(dmt_submit(DM_C2, 1, 100, 0, NULL));
	for (uint32_t i = 0; i < 3; i++) {
		zassert_ok(dmt_submit(DM_C3, 10 + i, 100, 0, NULL));
	}
	zassert_equal(dm_queue_bytes(DM_C3), 300);

	/* Over quota: the oldest C3 goes, C2 is untouched. */
	zassert_ok(dmt_submit(DM_C3, 13, 100, 0, NULL));
	zassert_equal(dm_queue_bytes(DM_C3), 300);
	zassert_equal(dm_queue_count(DM_C3), 3);
	zassert_equal(head_seq(DM_C3), 11, "oldest C3 should have been evicted");
	zassert_equal(dm_queue_count(DM_C2), 1);

	/* C0 has nothing below it to evict: refused, and counted as lost. */
	zassert_ok(dmt_submit(DM_C0, 20, 60, 0, NULL));
	zassert_equal(dmt_submit(DM_C0, 21, 60, 0, NULL), -ENOSPC);

	dm_stats_get(&s);
	zassert_equal(s.cls[DM_C3].dropped_quota, 1);
	zassert_equal(s.cls[DM_C3].submitted, 4);
	zassert_equal(s.cls[DM_C0].refused, 1);
	zassert_equal(s.c0_lost, 1);
	zassert_equal(s.pressure_state, DM_PRESSURE_CRITICAL);
	dm_stats_reset();
	zassert_equal(dm_pressure_get(), DM_PRESSURE_NORMAL);
}

/* Rules 1 and 3: shed order C4, C3, C2, C1; C0 is never touched. */
ZTEST(queue, test_shed_order)
{
	struct dm_stats s;
	uint32_t seq = 0;

	for (int i = 0; i < 10; i++) {
		zassert_ok(dmt_submit(DM_C4, seq++, 10, 0, NULL));
		zassert_ok(dmt_submit(DM_C3, seq++, 10, 0, NULL));
	}
	for (int i = 0; i < 6; i++) {
		zassert_ok(dmt_submit(DM_C2, seq++, 10, 0, NULL));
	}
	for (uint32_t k = 0; k < 4; k++) {
		struct dm_submit_opts o = {.coalesce_key = k, .link_hint = DM_LINK_ANY};

		zassert_ok(dmt_submit(DM_C1, seq++, 10, 0, &o));
	}
	zassert_ok(dmt_submit(DM_C0, seq++, 10, 0, NULL));
	zassert_ok(dmt_submit(DM_C0, seq++, 10, 0, NULL));
	zassert_equal(dm_slab_used(), CONFIG_DM_SLAB_COUNT, "slab should be full");

	/* Slab exhausted: a C2 submit sheds the oldest C4 once and succeeds. */
	zassert_ok(dmt_submit(DM_C2, seq++, 10, 0, NULL));
	zassert_equal(dm_queue_count(DM_C4), 9);
	zassert_equal(head_seq(DM_C4), 2, "oldest C4 should have gone first");

	zassert_true(dm_q_consumer_trylock());
	zassert_equal(dm_shed(0, 12, DM_C1), 12);
	zassert_equal(dm_queue_count(DM_C4), 0, "C4 is shed before C3");
	zassert_equal(dm_queue_count(DM_C3), 7);
	zassert_equal(dm_queue_count(DM_C2), 7);

	(void)dm_shed(0, 100, DM_C1);
	dm_q_consumer_unlock();
	zassert_equal(dm_queue_count(DM_C3), 0);
	zassert_equal(dm_queue_count(DM_C2), 0);
	zassert_equal(dm_queue_count(DM_C1), 4, "C1 is only shed by coalescing");
	zassert_equal(dm_queue_count(DM_C0), 2, "C0 is never shed");

	dm_stats_get(&s);
	zassert_equal(s.cls[DM_C4].dropped_shed, 10);
	zassert_equal(s.cls[DM_C3].dropped_shed, 10);
	zassert_equal(s.cls[DM_C2].dropped_shed, 7);
	zassert_equal(s.cls[DM_C0].dropped_shed, 0);
}

/* Rule 4: coalesce replaces in place, keeping the queue position. */
ZTEST(queue, test_coalesce_in_place)
{
	struct dm_submit_opts k7 = {.coalesce_key = 7, .link_hint = DM_LINK_ANY};
	struct dm_submit_opts k8 = {.coalesce_key = 8, .link_hint = DM_LINK_ANY};
	struct dm_msg_ref ref;
	struct dm_stats s;

	zassert_ok(dmt_submit(DM_C1, 1, 24, DM_F_COALESCE, &k7));
	zassert_ok(dmt_submit(DM_C1, 2, 24, DM_F_COALESCE, &k8));
	dm_test_clock_advance_ms(5);
	zassert_ok(dmt_submit(DM_C1, 3, 30, DM_F_COALESCE, &k7));

	zassert_equal(dm_queue_count(DM_C1), 2);
	zassert_equal(head_seq(DM_C1), 3, "newest key 7 should sit at the old position");
	zassert_true(dm_queue_head(DM_C1, &ref));
	zassert_equal(ref.len, 30, "length is updated");
	zassert_equal(ref.enq_tick, dm_now(), "tick is updated");
	zassert_equal(dm_queue_bytes(DM_C1), 54);
	zassert_equal(dm_slab_used(), 2, "the replaced block is freed");

	dm_stats_get(&s);
	zassert_equal(s.cls[DM_C1].coalesced, 1);
	zassert_equal(s.cls[DM_C1].submitted, 3);
}

/* Rule 5: age and deadline drops at the head; C0 is exempt. */
ZTEST(queue, test_age_drop)
{
	struct dm_submit_opts late = {.deadline_ms = 50, .link_hint = DM_LINK_ANY};
	struct dm_policy p;
	struct dm_stats s;

	dmt_policy(&p, 2);
	p.cls[DM_C0].max_age_ms = 10; /* ignored: control is never dropped */
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));

	zassert_ok(dmt_submit(DM_C2, 1, 50, 0, NULL)); /* C2 max_age is 300 ms */
	zassert_ok(dmt_submit(DM_C3, 2, 50, DM_F_DROP_LATE, &late));
	zassert_ok(dmt_submit(DM_C0, 3, 50, 0, NULL));
	zassert_true(dm_q_consumer_trylock());

	dm_test_clock_advance_ms(50);
	zassert_equal(dm_queue_age_sweep(dm_now()), 0);
	dm_test_clock_advance_ms(1);
	zassert_equal(dm_queue_age_sweep(dm_now()), 1, "C3 deadline passed");
	dm_test_clock_advance_ms(249);
	zassert_equal(dm_queue_age_sweep(dm_now()), 0, "C2 is exactly 300 ms old");
	dm_test_clock_advance_ms(1);
	zassert_equal(dm_queue_age_sweep(dm_now()), 1, "C2 is older than 300 ms");
	dm_test_clock_advance_ms(10000);
	zassert_equal(dm_queue_age_sweep(dm_now()), 0);
	dm_q_consumer_unlock();

	zassert_equal(dm_queue_count(DM_C0), 1, "C0 survives any age");
	dm_stats_get(&s);
	zassert_equal(s.cls[DM_C2].dropped_age, 1);
	zassert_equal(s.cls[DM_C3].dropped_age, 1);
}

/* Rule 6: refcnt 2 with two eligible links, else refcnt 1 + dup_pending. */
ZTEST(queue, test_dup_refcnt)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_msg_ref ref;
	const struct dm_msg_hdr *h;

	(void)dmt_add_link(0, &cfg);
	dmt_round(); /* nothing queued: only computes eligibility */
	zassert_ok(dmt_submit(DM_C0, 1, 16, 0, NULL));
	zassert_true(dm_queue_head(DM_C0, &ref));
	h = dm_blk_hdr(ref.slab_idx);
	zassert_equal(h->refcnt, 1);
	zassert_equal(h->dup_pending, 1, "one link: copy waits for a second");

	(void)dmt_add_link(1, &cfg);
	zassert_true(dm_q_consumer_trylock());
	zassert_true(dm_queue_pop(DM_C0, &ref));
	dm_q_consumer_unlock();
	dm_blk_free(ref.slab_idx);
	dmt_round();
	zassert_ok(dmt_submit(DM_C0, 2, 16, 0, NULL));
	zassert_true(dm_queue_head(DM_C0, &ref));
	h = dm_blk_hdr(ref.slab_idx);
	zassert_equal(h->refcnt, 2, "two eligible links: two copies");
	zassert_equal(h->dup_pending, 0);
}

/* Rule 7: from an ISR, anything O(depth) returns -EAGAIN instead. */
static volatile int isr_rc;
static volatile bool isr_ctx;
static struct {
	dm_class_t cls;
	uint32_t flags;
	size_t len;
} isr_req;

static void isr_submit(const void *arg)
{
	ARG_UNUSED(arg);
	isr_ctx = k_is_in_isr();
	isr_rc = dmt_submit(isr_req.cls, 7, isr_req.len, isr_req.flags, NULL);
}

static int submit_from_isr(dm_class_t c, size_t len, uint32_t flags)
{
	isr_req.cls = c;
	isr_req.len = len;
	isr_req.flags = flags;
	isr_rc = 1;
	isr_ctx = false;
	irq_offload(isr_submit, NULL);
	zassert_true(isr_ctx, "irq_offload did not run in ISR context");
	return isr_rc;
}

ZTEST(queue, test_isr_path)
{
	struct dm_policy p;

	zassert_equal(submit_from_isr(DM_C3, 20, 0), 0, "plain submit works from an ISR");
	zassert_equal(submit_from_isr(DM_C1, 20, DM_F_COALESCE), -EAGAIN);

	dm_test_q_busy(true);
	zassert_equal(submit_from_isr(DM_C3, 20, 0), -EAGAIN, "lock held: no spinning");
	dm_test_q_busy(false);

	dmt_policy(&p, 2);
	p.cls[DM_C4].quota_bytes = 100;
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	zassert_ok(dmt_submit(DM_C4, 1, 100, 0, NULL));
	zassert_equal(submit_from_isr(DM_C4, 10, 0), -EAGAIN, "over quota needs a shed");

	/* Fill the slab: C3 up to its depth, then C2. */
	while (dm_queue_count(DM_C3) < CONFIG_DM_Q_DEPTH_C3) {
		zassert_ok(dmt_submit(DM_C3, 2, 8, 0, NULL));
	}
	while (dm_slab_used() < CONFIG_DM_SLAB_COUNT) {
		zassert_ok(dmt_submit(DM_C2, 3, 8, 0, NULL));
	}
	zassert_equal(submit_from_isr(DM_C2, 10, 0), -EAGAIN, "slab empty needs a shed");
	zassert_equal(dm_slab_used(), CONFIG_DM_SLAB_COUNT, "nothing leaked");
}

ZTEST(queue, test_zero_copy)
{
	uint8_t *p = dm_alloc(64, K_NO_WAIT);
	uint8_t *q = dm_alloc(64, K_NO_WAIT);

	zassert_not_null(p);
	zassert_not_null(q);
	zassert_is_null(dm_alloc(DM_MAX_MSG + 1, K_NO_WAIT));
	memset(p, 0, 64);
	p[0] = DM_C3;
	zassert_ok(dm_submit_block(DM_C3, p, 64, 0, NULL));
	zassert_equal(dm_queue_count(DM_C3), 1);
	zassert_equal(dm_submit_block(DM_C3, p, 64, 0, NULL), -EINVAL, "already submitted");

	dm_free(q);
	zassert_equal(dm_slab_used(), 1);
	dm_free(p); /* queued, not owned: ignored */
	zassert_equal(dm_slab_used(), 1);
}
