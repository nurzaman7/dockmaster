/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * The scheduler thread (spec section 8). One thread serves every link. Each
 * round it settles completions, lets the link manager re-score, drops expired
 * heads, and then gives every idle link the highest class that has a
 * message, a wet bucket and affinity for that link.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

static struct dm_bucket_state bucket[DM_CLASS_COUNT][DM_MAX_LINKS];

static struct k_poll_signal sched_sig;
static struct k_timer round_timer;
static bool timer_running;
static uint32_t timer_period_ms;

K_MSGQ_DEFINE(dm_txc_q, sizeof(struct dm_txc), CONFIG_DM_LINK_SLOTS * 2, 4);

static K_THREAD_STACK_DEFINE(sched_stack, CONFIG_DM_SCHED_STACK);
static struct k_thread sched_thread;
static bool sched_thread_live;

/* Blocks whose duplicate copy still has to go out (rule 6). */
static uint16_t dup_list[DM_DUP_SLOTS];
static uint8_t dup_n;

/* ------------------------------------------------------------------ */
/* Buckets                                                             */
/* ------------------------------------------------------------------ */

void dm_bucket_refill(struct dm_bucket_state *b, const struct dm_bucket *p, uint32_t now)
{
	uint32_t dt = now - b->last_refill_tick; /* wraps correctly */
	int32_t cap = (int32_t)p->burst_ms * 1000;
	int64_t tokens;

	b->last_refill_tick = now;
	if (b->tokens_us >= cap) {
		b->tokens_us = cap;
		b->rem = 0U;
		return;
	}
	if (dt < k_us_to_ticks_floor32(4000000U)) {
		/* Common case, 32-bit only: Cortex-M3/M4 have no 64-bit divide. */
		uint32_t us = k_ticks_to_us_floor32(dt);
		uint32_t frac = (us % 1000U) * p->rate_ms_per_s + b->rem;

		tokens = (int64_t)b->tokens_us + (us / 1000U) * p->rate_ms_per_s + frac / 1000U;
		b->rem = frac % 1000U;
	} else {
		uint64_t num = k_ticks_to_us_floor64(dt) * p->rate_ms_per_s + b->rem;

		tokens = (int64_t)b->tokens_us + (int64_t)(num / 1000U);
		b->rem = (uint32_t)(num % 1000U);
	}
	if (tokens >= cap) {
		tokens = cap;
		b->rem = 0U;
	}
	b->tokens_us = (int32_t)tokens;
}

/*
 * Airtime is charged on completion: the estimate reserved at dispatch is
 * credited back and the measured airtime debited instead.
 */
void dm_bucket_settle(struct dm_bucket_state *b, const struct dm_bucket *p, uint32_t est_us,
		      uint32_t airtime_us)
{
	int64_t tokens = (int64_t)b->tokens_us + est_us - airtime_us;
	int64_t cap = (int64_t)p->burst_ms * 1000;

	b->tokens_us = (int32_t)MAX(MIN(tokens, cap), (int64_t)INT32_MIN);
}

struct dm_bucket_state *dm_sched_bucket(dm_class_t c, uint8_t l)
{
	return &bucket[c][l];
}

uint32_t dm_sched_refill_period_ms(const struct dm_policy *p)
{
	uint32_t smallest = UINT32_MAX;

	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		for (int l = 0; l < DM_MAX_LINKS; l++) {
			uint32_t burst = p->cls[c].bucket[l].burst_ms;

			if (burst != 0U && burst < smallest) {
				smallest = burst;
			}
		}
	}
	return smallest == UINT32_MAX ? DM_REFILL_MIN_MS : MAX(smallest / 4U, DM_REFILL_MIN_MS);
}

void dm_sched_link_added(uint8_t l, uint32_t now)
{
	const struct dm_policy *p = dm_policy_current();

	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		bucket[c][l].tokens_us = (int32_t)p->cls[c].bucket[l].burst_ms * 1000;
		bucket[c][l].last_refill_tick = now;
		bucket[c][l].rem = 0U;
	}
}

/* ------------------------------------------------------------------ */
/* Wake-ups                                                            */
/* ------------------------------------------------------------------ */

void dm_sched_kick(void)
{
	k_poll_signal_raise(&sched_sig, 0);
}

static void round_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	dm_sched_kick();
}

void dm_sched_policy_changed(void)
{
	timer_period_ms = dm_sched_refill_period_ms(dm_policy_current());
	if (timer_running) {
		k_timer_start(&round_timer, K_MSEC(timer_period_ms), K_MSEC(timer_period_ms));
	}
	dm_sched_kick();
}

void dm_sched_post(const struct dm_txc *t)
{
	(void)k_msgq_put(&dm_txc_q, t, K_FOREVER);
	dm_sched_kick();
}

/* ------------------------------------------------------------------ */
/* Duplicate copies                                                    */
/* ------------------------------------------------------------------ */

static void dup_add(uint16_t idx)
{
	for (uint8_t i = 0; i < dup_n; i++) {
		if (dup_list[i] == idx) {
			return;
		}
	}
	if (dup_n < DM_DUP_SLOTS) {
		dup_list[dup_n++] = idx;
		return;
	}
	/* No room to remember the second copy: give it up. */
	struct dm_msg_hdr *h = dm_blk_hdr(idx);
	struct dm_blk *b = dm_blk_meta(idx);

	h->refcnt = b->inflight;
	h->dup_pending = 0U;
}

static void dup_remove(uint16_t idx)
{
	for (uint8_t i = 0; i < dup_n; i++) {
		if (dup_list[i] == idx) {
			dup_list[i] = dup_list[--dup_n];
			return;
		}
	}
}

uint32_t dm_sched_dup_count(void)
{
	return dup_n;
}

/* Every copy of a block has completed (or was abandoned). */
static void finish(uint16_t idx)
{
	struct dm_msg_hdr *h = dm_blk_hdr(idx);
	struct dm_blk *b = dm_blk_meta(idx);
	dm_class_t c = (dm_class_t)h->cls;

	dup_remove(idx);
	if (b->ok_mask != 0U) {
		DM_CTR_INC(c, sent);
		dm_blk_free(idx);
		return;
	}
	if (c == DM_C0) {
		/* Control is never dropped: back to the front of C0. */
		b->sent_mask = 0U;
		b->state = DM_BLK_QUEUED;
		dm_queue_plan_copies(h);
		if (dm_queue_retry_c0(idx)) {
			return;
		}
		dm_pressure_c0_lost();
	} else if (c == DM_C1) {
		int rc = dm_queue_requeue_c1(idx);

		if (rc > 0) {
			return;
		}
		if (rc == 0) {
			DM_CTR_INC(c, coalesced); /* a newer value of the key exists */
		} else {
			DM_CTR_INC(c, failed);
		}
	} else {
		DM_CTR_INC(c, failed);
	}
	dm_blk_free(idx);
}

static void maybe_finish(uint16_t idx)
{
	const struct dm_msg_hdr *h = dm_blk_hdr(idx);

	if (h->refcnt == 0U && h->dup_pending == 0U) {
		finish(idx);
	}
}

static void dup_expire(uint32_t now)
{
	const struct dm_policy *p = dm_policy_current();

	for (uint8_t i = 0; i < dup_n;) {
		uint16_t idx = dup_list[i];
		struct dm_msg_hdr *h = dm_blk_hdr(idx);
		struct dm_blk *b = dm_blk_meta(idx);
		uint32_t win_ms = p->cls[h->cls].max_age_ms != 0U ? p->cls[h->cls].max_age_ms
								   : CONFIG_DM_DUP_WINDOW_MS;

		if ((int32_t)(now - b->enq_tick) <= (int32_t)dm_ms_to_ticks(win_ms)) {
			i++;
			continue;
		}
		h->dup_pending = 0U;
		h->refcnt = b->inflight; /* abandon copies not yet sent */
		dup_list[i] = dup_list[--dup_n];
		maybe_finish(idx);
	}
}

/* ------------------------------------------------------------------ */
/* Completions                                                         */
/* ------------------------------------------------------------------ */

static void on_txc(const struct dm_txc *t, uint32_t now)
{
	struct dm_link *l = &dm_links[t->link];
	const struct dm_policy *p = dm_policy_current();
	struct dm_msg_hdr *h = dm_blk_hdr(t->slab_idx);
	struct dm_blk *b = dm_blk_meta(t->slab_idx);
	struct dm_link_ctr *lc = &dm_ctr.link[t->link];

	if (l->inflight > 0U) {
		l->inflight--;
	}
	dm_bucket_settle(&bucket[t->cls][t->link], &p->cls[t->cls].bucket[t->link], t->est_us,
			 t->airtime_us);
	dm_stats_add_airtime((dm_class_t)t->cls, t->airtime_us);
	if (t->status == 0) {
		lc->tx_ok++;
		b->ok_mask |= BIT(t->link);
	} else {
		lc->tx_fail++;
	}
	if (t->fault != 0U) {
		lc->faults++;
	}
	dm_lm_on_tx(t->link, t->len, t->status, t->airtime_us, t->fault != 0U, now);
	DM_TRACE("dm_tx_done", t->cls, t->link, h->handle);

	if (b->inflight > 0U) {
		b->inflight--;
	}
	if (h->refcnt > 0U) {
		h->refcnt--;
	}
	maybe_finish(t->slab_idx);
}

static void drain_completions(uint32_t now)
{
	struct dm_txc t;

	while (k_msgq_get(&dm_txc_q, &t, K_NO_WAIT) == 0) {
		on_txc(&t, now);
	}
}

/* ------------------------------------------------------------------ */
/* Pick and dispatch                                                   */
/* ------------------------------------------------------------------ */

struct dm_pick {
	dm_class_t cls;
	uint16_t slab_idx;
	uint16_t len;
	uint32_t cost;
	bool from_dup;
};

static bool fits(const struct dm_link *l, const struct dm_msg_hdr *h, uint16_t len)
{
	return len <= l->caps.mtu || (h->flags & DM_F_RESUMABLE) != 0U;
}

/* Bucket gate: C0 may overdraw from a non-negative balance (section 8). */
static bool wet(dm_class_t c, uint8_t l, uint32_t cost, uint32_t now, bool *dry)
{
	struct dm_bucket_state *b = &bucket[c][l];

	dm_bucket_refill(b, &dm_policy_current()->cls[c].bucket[l], now);
	if (c == DM_C0 ? b->tokens_us >= 0 : b->tokens_us >= (int32_t)cost) {
		return true;
	}
	*dry = true;
	return false;
}

static bool pick_dup(struct dm_link *l, dm_class_t c, uint32_t now, struct dm_pick *out,
		     bool *dry)
{
	uint8_t elig = dm_lm_eligible(c);

	if ((elig & BIT(l->index)) == 0U) {
		return false;
	}
	for (uint8_t i = 0; i < dup_n; i++) {
		uint16_t idx = dup_list[i];
		struct dm_msg_hdr *h = dm_blk_hdr(idx);
		struct dm_blk *b = dm_blk_meta(idx);

		if (h->cls != c || (b->sent_mask & BIT(l->index)) != 0U || !fits(l, h, b->len)) {
			continue;
		}
		if (h->dup_pending != 0U) {
			/* A second link became eligible inside the window. */
			h->dup_pending = 0U;
			h->refcnt++;
		}
		if (h->refcnt <= b->inflight) {
			continue;
		}
		uint32_t cost = l->ops->cost_of_us(l->dev, b->len);

		if (!wet(c, l->index, cost, now, dry)) {
			return false;
		}
		*out = (struct dm_pick){.cls = c, .slab_idx = idx, .len = b->len, .cost = cost,
					.from_dup = true};
		return true;
	}
	return false;
}

static bool hinted_elsewhere(const struct dm_msg_hdr *h, dm_class_t c, uint8_t l)
{
	return h->link_hint != DM_LINK_ANY && h->link_hint != l &&
	       (dm_lm_eligible(c) & BIT(h->link_hint)) != 0U;
}

/* Strict priority across classes; the bucket decides if a class may go. */
static bool pick(struct dm_link *l, uint32_t now, struct dm_pick *out, bool *dry)
{
	for (int ci = DM_C0; ci < DM_CLASS_COUNT; ci++) {
		dm_class_t c = (dm_class_t)ci;
		struct dm_msg_ref head;

		if (pick_dup(l, c, now, out, dry)) {
			return true;
		}
		if (!dm_lm_serves(c, l->index) || !dm_queue_head(c, &head)) {
			continue;
		}
		const struct dm_msg_hdr *h = dm_blk_hdr(head.slab_idx);

		if (hinted_elsewhere(h, c, l->index) || !fits(l, h, head.len)) {
			continue;
		}
		uint32_t cost = l->ops->cost_of_us(l->dev, head.len);

		if (!wet(c, l->index, cost, now, dry)) {
			continue; /* dry: fall through to the next class */
		}
		*out = (struct dm_pick){.cls = c, .slab_idx = head.slab_idx, .len = head.len,
					.cost = cost, .from_dup = false};
		return true;
	}
	return false;
}

static void dispatch(struct dm_link *l, const struct dm_pick *pk)
{
	struct dm_msg_hdr *h = dm_blk_hdr(pk->slab_idx);
	struct dm_blk *b = dm_blk_meta(pk->slab_idx);
	struct dm_dispatch d = {
		.slab_idx = pk->slab_idx,
		.len = pk->len,
		.cls = (uint8_t)pk->cls,
		.kind = DM_DISP_TX,
		.handle = h->handle,
		.est_us = pk->cost,
	};

	if (!pk->from_dup) {
		struct dm_msg_ref ref;

		(void)dm_queue_pop(pk->cls, &ref);
		__ASSERT(ref.slab_idx == pk->slab_idx, "head moved during a round");
		b->state = DM_BLK_ACTIVE;
	}
	bucket[pk->cls][l->index].tokens_us -= (int32_t)pk->cost;
	b->inflight++;
	b->sent_mask |= BIT(l->index);
	l->inflight++;
	if (h->refcnt > b->inflight || h->dup_pending != 0U) {
		dup_add(pk->slab_idx);
	} else {
		dup_remove(pk->slab_idx);
	}
	DM_TRACE("dm_sched_pick", pk->cls, l->index, h->handle);
	if (k_msgq_put(&l->dispatch_q, &d, K_NO_WAIT) != 0) {
		/* Cannot happen: a link is only picked when idle. */
		__ASSERT(false, "link %u: owner queue full", l->index);
	}
}

static void dispatch_probes(void)
{
	for (uint8_t i = 0; i < dm_n_links; i++) {
		struct dm_link *l = &dm_links[i];
		struct dm_dispatch d = {.slab_idx = DM_NO_SLAB, .kind = DM_DISP_PROBE};

		if (!l->probe_req || l->probe_inflight || l->ops->probe == NULL) {
			continue;
		}
		if (k_msgq_put(&l->dispatch_q, &d, K_NO_WAIT) == 0) {
			l->probe_req = false;
			l->probe_inflight = true;
			l->probe_sent_tick = dm_now();
		}
	}
}

/* ------------------------------------------------------------------ */
/* One round                                                           */
/* ------------------------------------------------------------------ */

static void update_timer(bool pending)
{
	if (pending && !timer_running) {
		k_timer_start(&round_timer, K_MSEC(timer_period_ms), K_MSEC(timer_period_ms));
		timer_running = true;
	} else if (!pending && timer_running) {
		k_timer_stop(&round_timer);
		timer_running = false;
	}
}

void dm_sched_round(uint32_t now)
{
	uint32_t t0 = k_cycle_get_32();
	bool dry = false;

	drain_completions(now);
	dm_lm_service(now);
	dm_policy_service();
	dm_pressure_service();
	dispatch_probes();

	if (dm_q_consumer_trylock()) {
		(void)dm_queue_age_sweep(now);
		dup_expire(now);
		if (dm_pressure_get() >= DM_PRESSURE_SHED) {
			const struct dm_policy *p = dm_policy_current();
			uint32_t target = (uint32_t)CONFIG_DM_SLAB_COUNT * p->depth_tighten_pct[1] / 100U;
			uint32_t used = dm_slab_used();

			if (used >= target) {
				(void)dm_shed(0U, used - target + 1U, DM_C1);
			}
		}
		for (uint8_t i = 0; i < dm_n_links; i++) {
			struct dm_link *l = &dm_links[i];
			struct dm_pick pk;

			if (!l->started || l->inflight != 0U ||
			    dm_lm_state(i) == DM_LINK_DOWN) {
				continue;
			}
			if (pick(l, now, &pk, &dry)) {
				dispatch(l, &pk);
			}
		}
		dm_q_consumer_unlock();
	}

	bool probing = false;

	for (uint8_t i = 0; i < dm_n_links; i++) {
		probing = probing || dm_links[i].probe_inflight;
	}
	update_timer(dm_queue_any() || dup_n != 0U || probing);

	uint32_t cyc = k_cycle_get_32() - t0;

	dm_ctr.sched_rounds++;
	if (cyc > dm_ctr.sched_max_cyc) {
		dm_ctr.sched_max_cyc = cyc;
	}
}

static void sched_loop(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct k_poll_event evt =
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &sched_sig);

	for (;;) {
		(void)k_poll(&evt, 1, K_FOREVER);
		evt.state = K_POLL_STATE_NOT_READY;
		k_poll_signal_reset(&sched_sig);
		dm_sched_round(dm_now());
	}
}

void dm_sched_init(void)
{
	k_poll_signal_init(&sched_sig);
	k_timer_init(&round_timer, round_timer_fn, NULL);
	timer_running = false;
	timer_period_ms = dm_sched_refill_period_ms(dm_policy_current());
	k_msgq_purge(&dm_txc_q);
	memset(bucket, 0, sizeof(bucket));
	dup_n = 0U;
}

void dm_sched_start(bool thread)
{
	if (!thread || sched_thread_live) {
		return;
	}
	k_thread_create(&sched_thread, sched_stack, K_THREAD_STACK_SIZEOF(sched_stack), sched_loop,
			NULL, NULL, NULL, CONFIG_DM_SCHED_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&sched_thread, "dm_sched");
	sched_thread_live = true;
}

void dm_sched_stop(void)
{
	if (sched_thread_live) {
		k_thread_abort(&sched_thread);
		sched_thread_live = false;
	}
	k_timer_stop(&round_timer);
	timer_running = false;
}
