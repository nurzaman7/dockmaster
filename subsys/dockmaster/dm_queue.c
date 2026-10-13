/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slabs, class queues, submit, coalesce, shed and age (spec section 7).
 *
 * Concurrency model (single core):
 *  - Every ring operation is O(1) or a bounded shift, done under irq_lock.
 *  - Producers (threads and ISRs) only append at the tail, or, from a thread,
 *    swap one entry in place for coalescing.
 *  - Popping from the head (dispatch, age, shed) needs the consumer lock, an
 *    atomic flag taken with try semantics. Nobody ever waits on it: the
 *    scheduler skips a round, a producer skips the shed, an ISR gets -EAGAIN.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

BUILD_ASSERT(CONFIG_DM_MAX_MSG <= CONFIG_DM_SLAB_BLOCK - DM_MSG_HDR_SIZE,
	     "CONFIG_DM_MAX_MSG must leave room for the 16 byte header");
BUILD_ASSERT(CONFIG_DM_SLAB_BLOCK % 4 == 0, "slab blocks must be word aligned");
BUILD_ASSERT(CONFIG_DM_SLAB_COUNT < DM_NO_SLAB, "slab index must fit 16 bits");

#if defined(CONFIG_DM_NOCACHE_SECTION)
#define DM_SLAB_SECTION __nocache
#else
#define DM_SLAB_SECTION
#endif

static uint8_t dm_slab_buf[CONFIG_DM_SLAB_COUNT * CONFIG_DM_SLAB_BLOCK] __aligned(32)
	DM_SLAB_SECTION;
struct k_mem_slab dm_slab;
static struct dm_blk dm_blks[CONFIG_DM_SLAB_COUNT];

static struct dm_msg_ref q0_buf[CONFIG_DM_Q_DEPTH_C0];
static struct dm_msg_ref q1_buf[CONFIG_DM_Q_DEPTH_C1];
static struct dm_msg_ref q2_buf[CONFIG_DM_Q_DEPTH_C2];
static struct dm_msg_ref q3_buf[CONFIG_DM_Q_DEPTH_C3];
static struct dm_msg_ref q4_buf[CONFIG_DM_Q_DEPTH_C4];
static struct dm_msg_ref retry_buf[CONFIG_DM_LINK_SLOTS];

static struct dm_ring dm_q[DM_CLASS_COUNT];
static struct dm_ring c0_retry;

static atomic_t cls_bytes[DM_CLASS_COUNT];
static atomic_t q_busy;
static atomic_t next_handle;
static bool q_ready;
static uint32_t shed_log_tick;

/* ------------------------------------------------------------------ */
/* Rings                                                               */
/* ------------------------------------------------------------------ */

static inline uint8_t ring_idx(const struct dm_ring *r, uint32_t i)
{
	uint32_t idx = r->head + i;

	return (uint8_t)(idx >= r->cap ? idx - r->cap : idx);
}

/* Returns the count before the put, or -1 when full. */
static int ring_put(struct dm_ring *r, const struct dm_msg_ref *ref)
{
	unsigned int key = irq_lock();
	int before = r->count;

	if (r->count < r->cap) {
		r->buf[ring_idx(r, r->count)] = *ref;
		r->count++;
	} else {
		before = -1;
	}
	irq_unlock(key);
	return before;
}

static bool ring_put_front(struct dm_ring *r, const struct dm_msg_ref *ref)
{
	unsigned int key = irq_lock();
	bool ok = r->count < r->cap;

	if (ok) {
		r->head = (r->head == 0U) ? (uint8_t)(r->cap - 1U) : (uint8_t)(r->head - 1U);
		r->buf[r->head] = *ref;
		r->count++;
	}
	irq_unlock(key);
	return ok;
}

static bool ring_get(struct dm_ring *r, struct dm_msg_ref *out)
{
	unsigned int key = irq_lock();
	bool ok = r->count > 0U;

	if (ok) {
		*out = r->buf[r->head];
		r->head = ring_idx(r, 1U);
		r->count--;
	}
	irq_unlock(key);
	return ok;
}

static bool ring_peek_at(struct dm_ring *r, uint32_t i, struct dm_msg_ref *out)
{
	unsigned int key = irq_lock();
	bool ok = i < r->count;

	if (ok) {
		*out = r->buf[ring_idx(r, i)];
	}
	irq_unlock(key);
	return ok;
}

/* Swap entry @p i for @p ref if it still references @p expect. */
static bool ring_replace_at(struct dm_ring *r, uint32_t i, uint16_t expect,
			    const struct dm_msg_ref *ref)
{
	unsigned int key = irq_lock();
	bool ok = i < r->count && r->buf[ring_idx(r, i)].slab_idx == expect;

	if (ok) {
		r->buf[ring_idx(r, i)] = *ref;
	}
	irq_unlock(key);
	return ok;
}

/* Remove entry @p i if it still references @p expect; shifts at most depth. */
static bool ring_remove_at(struct dm_ring *r, uint32_t i, uint16_t expect)
{
	unsigned int key = irq_lock();
	bool ok = i < r->count && r->buf[ring_idx(r, i)].slab_idx == expect;

	if (ok) {
		for (uint32_t j = i; j + 1U < r->count; j++) {
			r->buf[ring_idx(r, j)] = r->buf[ring_idx(r, j + 1U)];
		}
		r->count--;
	}
	irq_unlock(key);
	return ok;
}

static void ring_init(struct dm_ring *r, struct dm_msg_ref *buf, uint8_t cap)
{
	r->buf = buf;
	r->cap = cap;
	r->head = 0U;
	r->count = 0U;
}

/* ------------------------------------------------------------------ */
/* Blocks                                                              */
/* ------------------------------------------------------------------ */

void dm_queue_init(void)
{
	(void)k_mem_slab_init(&dm_slab, dm_slab_buf, CONFIG_DM_SLAB_BLOCK, CONFIG_DM_SLAB_COUNT);
	memset(dm_blks, 0, sizeof(dm_blks));
	ring_init(&dm_q[DM_C0], q0_buf, CONFIG_DM_Q_DEPTH_C0);
	ring_init(&dm_q[DM_C1], q1_buf, CONFIG_DM_Q_DEPTH_C1);
	ring_init(&dm_q[DM_C2], q2_buf, CONFIG_DM_Q_DEPTH_C2);
	ring_init(&dm_q[DM_C3], q3_buf, CONFIG_DM_Q_DEPTH_C3);
	ring_init(&dm_q[DM_C4], q4_buf, CONFIG_DM_Q_DEPTH_C4);
	ring_init(&c0_retry, retry_buf, CONFIG_DM_LINK_SLOTS);
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		atomic_set(&cls_bytes[c], 0);
	}
	atomic_set(&q_busy, 0);
	q_ready = true;
}

bool dm_queue_ready(void)
{
	return q_ready;
}

static inline uint8_t *blk_base(uint16_t idx)
{
	return &dm_slab_buf[(size_t)idx * CONFIG_DM_SLAB_BLOCK];
}

struct dm_msg_hdr *dm_blk_hdr(uint16_t idx)
{
	return (struct dm_msg_hdr *)blk_base(idx);
}

uint8_t *dm_blk_payload(uint16_t idx)
{
	return blk_base(idx) + DM_MSG_HDR_SIZE;
}

struct dm_blk *dm_blk_meta(uint16_t idx)
{
	return &dm_blks[idx];
}

static int blk_index(const void *mem)
{
	ptrdiff_t off = (const uint8_t *)mem - dm_slab_buf;

	if (off < 0 || (off % CONFIG_DM_SLAB_BLOCK) != 0 ||
	    off / CONFIG_DM_SLAB_BLOCK >= CONFIG_DM_SLAB_COUNT) {
		return -EINVAL;
	}
	return (int)(off / CONFIG_DM_SLAB_BLOCK);
}

void dm_blk_free(uint16_t idx)
{
	struct dm_blk *b = &dm_blks[idx];

	if (b->accounted) {
		atomic_sub(&cls_bytes[dm_blk_hdr(idx)->cls], b->len);
	}
	memset(b, 0, sizeof(*b));
	k_mem_slab_free(&dm_slab, blk_base(idx));
}

static bool slab_alloc(uint16_t *out)
{
	void *mem;

	if (k_mem_slab_alloc(&dm_slab, &mem, K_NO_WAIT) != 0) {
		return false;
	}
	*out = (uint16_t)blk_index(mem);
	return true;
}

uint32_t dm_slab_used(void)
{
	return k_mem_slab_num_used_get(&dm_slab);
}

uint8_t dm_slab_pct(void)
{
	return (uint8_t)((dm_slab_used() * 100U) / CONFIG_DM_SLAB_COUNT);
}

uint32_t dm_slab_capacity_bytes(void)
{
	return (uint32_t)CONFIG_DM_SLAB_COUNT * CONFIG_DM_MAX_MSG;
}

/* ------------------------------------------------------------------ */
/* Consumer side                                                       */
/* ------------------------------------------------------------------ */

bool dm_q_consumer_trylock(void)
{
	return atomic_cas(&q_busy, 0, 1);
}

void dm_q_consumer_unlock(void)
{
	atomic_set(&q_busy, 0);
}

bool dm_queue_head(dm_class_t c, struct dm_msg_ref *out)
{
	if (c == DM_C0 && ring_peek_at(&c0_retry, 0U, out)) {
		return true;
	}
	return ring_peek_at(&dm_q[c], 0U, out);
}

bool dm_queue_pop(dm_class_t c, struct dm_msg_ref *out)
{
	if (c == DM_C0 && ring_get(&c0_retry, out)) {
		return true;
	}
	return ring_get(&dm_q[c], out);
}

bool dm_queue_retry_c0(uint16_t idx)
{
	const struct dm_blk *b = &dm_blks[idx];
	struct dm_msg_ref ref = {.slab_idx = idx, .len = b->len, .enq_tick = b->enq_tick};

	if (ring_put(&c0_retry, &ref) >= 0) {
		return true;
	}
	return ring_put_front(&dm_q[DM_C0], &ref);
}

/*
 * C1 is state: the newest value of each key must get through. A C1 message
 * that failed on every link goes back in the queue unless a newer message
 * with the same key exists (queued or in flight); max_age still bounds it.
 */
int dm_queue_requeue_c1(uint16_t idx)
{
	struct dm_msg_hdr *h = dm_blk_hdr(idx);
	struct dm_blk *b = &dm_blks[idx];

	if ((h->flags & DM_F_COALESCE) == 0U) {
		return -ENOENT;
	}
	for (uint16_t i = 0; i < CONFIG_DM_SLAB_COUNT; i++) {
		const struct dm_msg_hdr *o = dm_blk_hdr(i);

		if (i != idx && dm_blks[i].state >= DM_BLK_QUEUED && o->cls == DM_C1 &&
		    (o->flags & DM_F_COALESCE) != 0U && o->coalesce_key == h->coalesce_key &&
		    (int16_t)(o->handle - h->handle) > 0) {
			return 0; /* superseded */
		}
	}
	struct dm_msg_ref ref = {.slab_idx = idx, .len = b->len, .enq_tick = b->enq_tick};

	b->sent_mask = 0U;
	b->ok_mask = 0U;
	b->state = DM_BLK_QUEUED;
	h->refcnt = 1U;
	h->dup_pending = 0U;
	if (ring_put(&dm_q[DM_C1], &ref) < 0) {
		b->state = DM_BLK_ACTIVE;
		return -ENOSPC;
	}
	return 1;
}

uint32_t dm_queue_count(dm_class_t c)
{
	return dm_q[c].count + (c == DM_C0 ? c0_retry.count : 0U);
}

bool dm_queue_any(void)
{
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		if (dm_queue_count((dm_class_t)c) != 0U) {
			return true;
		}
	}
	return false;
}

uint32_t dm_queue_bytes(dm_class_t c)
{
	return (uint32_t)atomic_get(&cls_bytes[c]);
}

static void count_drop(dm_class_t c, enum dm_drop_reason why, uint16_t handle)
{
	switch (why) {
	case DM_DROP_QUOTA:
		DM_CTR_INC(c, dropped_quota);
		break;
	case DM_DROP_SHED:
		DM_CTR_INC(c, dropped_shed);
		break;
	case DM_DROP_AGE:
		DM_CTR_INC(c, dropped_age);
		break;
	}
	DM_TRACE("dm_shed", c, DM_NO_LINK, handle);
	if (why != DM_DROP_AGE && dm_log_ratelimit(&shed_log_tick)) {
		LOG_WRN("shed C%d (%s)", c, why == DM_DROP_QUOTA ? "quota" : "pressure");
	}
}

/* Evict the oldest message of class @p c. Caller holds the consumer lock. */
static uint32_t evict_oldest(dm_class_t c, enum dm_drop_reason why)
{
	struct dm_msg_ref ref;

	if (!ring_get(&dm_q[c], &ref)) {
		return 0U;
	}
	count_drop(c, why, dm_blk_hdr(ref.slab_idx)->handle);
	dm_blk_free(ref.slab_idx);
	return ref.len;
}

/*
 * C1 is shed by coalescing: drop older messages whose key reappears later in
 * the queue. Returns bytes freed. Caller holds the consumer lock.
 */
static uint32_t coalesce_c1(uint32_t need_bytes, enum dm_drop_reason why)
{
	struct dm_ring *r = &dm_q[DM_C1];
	uint32_t freed = 0U;
	uint32_t i = 0U;

	while (freed < need_bytes || need_bytes == 0U) {
		struct dm_msg_ref a;
		bool dup = false;

		if (!ring_peek_at(r, i, &a)) {
			break;
		}
		uint32_t key = dm_blk_hdr(a.slab_idx)->coalesce_key;

		for (uint32_t j = i + 1U;; j++) {
			struct dm_msg_ref b;

			if (!ring_peek_at(r, j, &b)) {
				break;
			}
			if (dm_blk_hdr(b.slab_idx)->coalesce_key == key) {
				dup = true;
				break;
			}
		}
		if (dup && ring_remove_at(r, i, a.slab_idx)) {
			count_drop(DM_C1, why, dm_blk_hdr(a.slab_idx)->handle);
			DM_CTR_INC(DM_C1, coalesced);
			dm_blk_free(a.slab_idx);
			freed += a.len;
			continue; /* same index now holds the next entry */
		}
		i++;
	}
	return freed;
}

uint32_t dm_shed(uint32_t need_bytes, uint32_t need_blocks, dm_class_t floor)
{
	uint32_t bytes = 0U;
	uint32_t blocks = 0U;

	if (floor < DM_C1) {
		floor = DM_C1; /* C0 is never shed */
	}
	for (int c = DM_C4; c >= (int)floor; c--) {
		while (bytes < need_bytes || blocks < need_blocks) {
			uint32_t got;

			if (c == DM_C1) {
				uint32_t before = dm_slab_used();

				got = coalesce_c1(need_bytes > bytes ? need_bytes - bytes : 1U,
						  DM_DROP_SHED);
				blocks += before - dm_slab_used();
				bytes += got;
				break;
			}
			got = evict_oldest((dm_class_t)c, DM_DROP_SHED);
			if (got == 0U) {
				break;
			}
			bytes += got;
			blocks++;
		}
		if (bytes >= need_bytes && blocks >= need_blocks) {
			break;
		}
	}
	return blocks;
}

/* Shed inside one class to honour its quota (rule 2). */
static void shed_within(dm_class_t c, uint32_t need_bytes)
{
	uint32_t freed = 0U;

	if (c == DM_C0 || !dm_q_consumer_trylock()) {
		return;
	}
	if (c == DM_C1) {
		(void)coalesce_c1(need_bytes, DM_DROP_QUOTA);
	} else {
		while (freed < need_bytes) {
			uint32_t got = evict_oldest(c, DM_DROP_QUOTA);

			if (got == 0U) {
				break;
			}
			freed += got;
		}
	}
	dm_q_consumer_unlock();
}

uint32_t dm_queue_age_sweep(uint32_t now)
{
	const struct dm_policy *p = dm_policy_current();
	uint32_t dropped = 0U;

	/* C0 is never dropped, whatever its policy says. */
	for (int c = DM_C1; c < DM_CLASS_COUNT; c++) {
		uint32_t max_age = dm_ms_to_ticks(p->cls[c].max_age_ms);
		struct dm_msg_ref ref;

		while (ring_peek_at(&dm_q[c], 0U, &ref)) {
			const struct dm_msg_hdr *h = dm_blk_hdr(ref.slab_idx);
			bool old = p->cls[c].max_age_ms != 0U &&
				   (int32_t)(now - ref.enq_tick) > (int32_t)max_age;
			bool late = (h->flags & DM_HF_DEADLINE) != 0U &&
				    (int32_t)(now - h->deadline_tick) > 0;

			if (!old && !late) {
				break;
			}
			(void)ring_get(&dm_q[c], &ref);
			count_drop((dm_class_t)c, DM_DROP_AGE, h->handle);
			dm_blk_free(ref.slab_idx);
			dropped++;
		}
	}
	return dropped;
}

/* ------------------------------------------------------------------ */
/* Submit                                                              */
/* ------------------------------------------------------------------ */

static const struct dm_submit_opts opts_default = {.link_hint = DM_LINK_ANY};

static bool wants_coalesce(dm_class_t c, uint32_t flags, const struct dm_policy *p)
{
	return (flags & DM_F_COALESCE) != 0U || (p->cls[c].flags & DM_P_COALESCE) != 0U ||
	       dm_pressure_forces_coalesce(c);
}

/* Swap a queued message with the same key for @p idx. Returns old length. */
static int try_coalesce(dm_class_t c, uint16_t idx, uint16_t len, uint32_t key, uint32_t now)
{
	struct dm_ring *r = &dm_q[c];
	struct dm_msg_ref ref;

	for (uint32_t i = 0U; ring_peek_at(r, i, &ref); i++) {
		const struct dm_msg_hdr *h = dm_blk_hdr(ref.slab_idx);

		if ((h->flags & DM_F_COALESCE) == 0U || h->coalesce_key != key) {
			continue;
		}
		struct dm_msg_ref fresh = {.slab_idx = idx, .len = len, .enq_tick = now};

		if (ring_replace_at(r, i, ref.slab_idx, &fresh)) {
			DM_CTR_INC(c, coalesced);
			dm_blk_free(ref.slab_idx);
			return ref.len;
		}
		break; /* the entry moved under us; append instead */
	}
	return -ENOENT;
}

/* Decide how many copies a message needs (rule 6). */
void dm_queue_plan_copies(struct dm_msg_hdr *h)
{
	bool dup = (h->flags & DM_F_DUP) != 0U || dm_lm_dup_class((dm_class_t)h->cls);
	uint8_t n = (uint8_t)POPCOUNT(dm_lm_eligible((dm_class_t)h->cls));

	h->refcnt = (dup && n >= 2U) ? 2U : 1U;
	h->dup_pending = (dup && n < 2U) ? 1U : 0U;
}

static int fail_block(uint16_t idx, int rc)
{
	dm_blk_free(idx);
	return rc;
}

static int enqueue(dm_class_t c, uint16_t idx, size_t len, uint32_t flags,
		   const struct dm_submit_opts *o, bool isr)
{
	const struct dm_policy *p = dm_policy_current();
	struct dm_msg_hdr *h = dm_blk_hdr(idx);
	struct dm_blk *b = &dm_blks[idx];
	uint32_t now = dm_now();
	bool coalesce = wants_coalesce(c, flags, p);
	uint32_t quota = p->cls[c].quota_bytes;

	if (isr && (coalesce || atomic_get(&q_busy) != 0)) {
		return fail_block(idx, -EAGAIN);
	}

	h->cls = (uint8_t)c;
	h->flags = (uint8_t)(flags & (DM_F_COALESCE | DM_F_DROP_LATE | DM_F_RESUMABLE | DM_F_DUP));
	if (coalesce) {
		h->flags |= DM_F_COALESCE;
	}
	if ((flags & DM_F_DROP_LATE) != 0U && o->deadline_ms != 0U) {
		h->flags |= DM_HF_DEADLINE;
		h->deadline_tick = now + dm_ms_to_ticks(o->deadline_ms);
	} else {
		h->deadline_tick = 0U;
	}
	h->coalesce_key = o->coalesce_key;
	h->link_hint = o->link_hint;
	h->handle = (uint16_t)atomic_inc(&next_handle);
	dm_queue_plan_copies(h);
	b->len = (uint16_t)len;
	b->enq_tick = now;
	b->state = DM_BLK_QUEUED;
	b->accounted = true;

	if (coalesce) {
		int old = try_coalesce(c, idx, (uint16_t)len, o->coalesce_key, now);

		if (old >= 0) {
			atomic_add(&cls_bytes[c], (atomic_val_t)len);
			if ((uint32_t)atomic_get(&cls_bytes[c]) > quota) {
				shed_within(c, (uint32_t)atomic_get(&cls_bytes[c]) - quota);
			}
			DM_CTR_INC(c, submitted);
			DM_TRACE("dm_submit", c, DM_LINK_ANY, h->handle);
			dm_pressure_on_submit();
			return 0;
		}
	}

	/* Rule 2: reserve the bytes, shed inside the class if over quota. */
	uint32_t before = (uint32_t)atomic_add(&cls_bytes[c], (atomic_val_t)len);

	if (before + len > quota) {
		if (isr) {
			b->accounted = false;
			atomic_sub(&cls_bytes[c], (atomic_val_t)len);
			return fail_block(idx, -EAGAIN);
		}
		shed_within(c, before + len - quota);
		if ((uint32_t)atomic_get(&cls_bytes[c]) > quota) {
			return fail_block(idx, -ENOSPC);
		}
	}

	struct dm_msg_ref ref = {.slab_idx = idx, .len = (uint16_t)len, .enq_tick = now};
	int was = ring_put(&dm_q[c], &ref);

	if (was < 0 && !isr) {
		shed_within(c, 1U);
		was = ring_put(&dm_q[c], &ref);
	}
	if (was < 0) {
		return fail_block(idx, isr ? -EAGAIN : -ENOSPC);
	}

	DM_CTR_INC(c, submitted);
	DM_TRACE("dm_submit", c, DM_LINK_ANY, h->handle);
	if (was == 0) {
		dm_sched_kick();
	}
	dm_pressure_on_submit();
	return 0;
}

static int check_args(dm_class_t c, size_t len, const struct dm_submit_opts *o)
{
	if ((unsigned int)c >= DM_CLASS_COUNT || len == 0U || len > DM_MAX_MSG) {
		return -EINVAL;
	}
	if (o->link_hint != DM_LINK_ANY && o->link_hint >= dm_n_links) {
		return -EINVAL;
	}
	if (!q_ready) {
		return -EAGAIN;
	}
	return 0;
}

/* Count a refusal; a thread-context C0 refusal is a lost control message. */
static int note_result(dm_class_t c, int rc, bool isr)
{
	if (rc == 0 || rc == -EINVAL) {
		return rc;
	}
	DM_CTR_INC(c, refused);
	if (c == DM_C0 && !isr && (rc == -ENOMEM || rc == -ENOSPC)) {
		dm_pressure_c0_lost();
	}
	return rc;
}

int dm_submit(dm_class_t cls, const void *buf, size_t len, uint32_t flags,
	      const struct dm_submit_opts *opts)
{
	const struct dm_submit_opts *o = opts != NULL ? opts : &opts_default;
	bool isr = k_is_in_isr();
	uint16_t idx;
	int rc = check_args(cls, len, o);

	if (rc != 0 || buf == NULL) {
		return rc != 0 ? rc : -EINVAL;
	}
	if (dm_pressure_refuses(cls)) {
		return note_result(cls, -ENOSPC, isr);
	}
	if (isr && wants_coalesce(cls, flags, dm_policy_current())) {
		return note_result(cls, -EAGAIN, isr); /* rule 7: no O(depth) work in an ISR */
	}

	/* Rule 1: one shed and one retry on slab exhaustion. */
	if (!slab_alloc(&idx)) {
		if (isr) {
			return note_result(cls, -EAGAIN, isr);
		}
		if (dm_q_consumer_trylock()) {
			(void)dm_shed(0U, 1U, cls == DM_C0 ? DM_C1 : cls);
			dm_q_consumer_unlock();
		}
		if (!slab_alloc(&idx)) {
			return note_result(cls, -ENOMEM, isr);
		}
	}
	memcpy(dm_blk_payload(idx), buf, len);
	return note_result(cls, enqueue(cls, idx, len, flags, o, isr), isr);
}

void *dm_alloc(size_t len, k_timeout_t timeout)
{
	void *mem;

	if (len > DM_MAX_MSG || !q_ready) {
		return NULL;
	}
	if (k_mem_slab_alloc(&dm_slab, &mem, timeout) != 0) {
		return NULL;
	}
	dm_blks[blk_index(mem)].state = DM_BLK_OWNED;
	return (uint8_t *)mem + DM_MSG_HDR_SIZE;
}

void dm_free(void *block)
{
	int idx = (block != NULL) ? blk_index((uint8_t *)block - DM_MSG_HDR_SIZE) : -EINVAL;

	if (idx >= 0 && dm_blks[idx].state == DM_BLK_OWNED) {
		dm_blk_free((uint16_t)idx);
	}
}

int dm_submit_block(dm_class_t cls, void *block, size_t len, uint32_t flags,
		    const struct dm_submit_opts *opts)
{
	const struct dm_submit_opts *o = opts != NULL ? opts : &opts_default;
	int idx = (block != NULL) ? blk_index((uint8_t *)block - DM_MSG_HDR_SIZE) : -EINVAL;
	bool isr = k_is_in_isr();
	int rc;

	if (idx < 0 || dm_blks[idx].state != DM_BLK_OWNED) {
		return -EINVAL;
	}
	rc = check_args(cls, len, o);
	if (rc == 0 && dm_pressure_refuses(cls)) {
		rc = -ENOSPC;
	}
	if (rc != 0) {
		(void)fail_block((uint16_t)idx, rc);
		return note_result(cls, rc, isr);
	}
	return note_result(cls, enqueue(cls, (uint16_t)idx, len, flags, o, isr), isr);
}

#if defined(CONFIG_DM_TEST_HOOKS)
void dm_test_q_busy(bool busy)
{
	atomic_set(&q_busy, busy ? 1 : 0);
}
#endif
