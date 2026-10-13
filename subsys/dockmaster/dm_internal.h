/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private interfaces shared by the Dockmaster core files. Nothing here is
 * stable API; tests include it to reach the pure functions and test hooks.
 */

#ifndef DOCKMASTER_DM_INTERNAL_H_
#define DOCKMASTER_DM_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <dockmaster/dm.h>
#include <dockmaster/dm_link.h>
#include <dockmaster/dm_stats.h>

#if defined(CONFIG_DM_TRACING)
#include <zephyr/tracing/tracing.h>
#endif

BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "Dockmaster assumes a single core");

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define DM_MSG_HDR_SIZE     16U
#define DM_NO_LINK          0xFFU
#define DM_NO_SLAB          0xFFFFU
#define DM_DUP_SLOTS        8U
#define DM_PROBE_TIMEOUT_MS 2000U
#define DM_RECOVERY_MS      5000U
#define DM_WATCHDOG_MIN_US  1000U
#define DM_REFILL_MIN_MS    10U

/* Private header flag: the message carries a deadline. */
#define DM_HF_DEADLINE BIT(7)

/* ------------------------------------------------------------------ */
/* Messages and queues (dm_queue.c)                                    */
/* ------------------------------------------------------------------ */

/** What sits in a class queue: 8 bytes. */
struct dm_msg_ref {
	uint16_t slab_idx;
	uint16_t len;
	uint32_t enq_tick; /* low 32 bits of the tick counter at submit */
};
BUILD_ASSERT(sizeof(struct dm_msg_ref) == 8);

/** First 16 bytes of every slab block. */
struct dm_msg_hdr {
	uint32_t deadline_tick;
	uint32_t coalesce_key;
	uint8_t cls;
	uint8_t flags;
	uint8_t link_hint;
	uint8_t dup_pending; /* a second copy waits for a second eligible link */
	uint16_t refcnt;     /* copies not yet completed (queued or in flight) */
	uint16_t handle;     /* message handle for tracing */
};
BUILD_ASSERT(sizeof(struct dm_msg_hdr) == DM_MSG_HDR_SIZE);

enum dm_blk_state {
	DM_BLK_FREE = 0,
	DM_BLK_OWNED,  /* handed out by dm_alloc() */
	DM_BLK_QUEUED, /* referenced from a class queue */
	DM_BLK_ACTIVE, /* popped: in flight and/or waiting for a duplicate copy */
};

/** RAM-side bookkeeping for one slab block. Scheduler owned once popped. */
struct dm_blk {
	uint32_t enq_tick;
	uint16_t len;
	uint8_t state;
	uint8_t sent_mask; /* links this block was handed to */
	uint8_t ok_mask;   /* links that reported success */
	uint8_t inflight;  /* copies dispatched and not completed */
	bool accounted;    /* len is counted in the class byte total */
};

/** Fixed ring of message references. */
struct dm_ring {
	struct dm_msg_ref *buf;
	uint8_t cap;
	uint8_t head;
	uint8_t count;
};

void dm_queue_init(void);
bool dm_queue_ready(void);
struct dm_msg_hdr *dm_blk_hdr(uint16_t idx);
uint8_t *dm_blk_payload(uint16_t idx);
struct dm_blk *dm_blk_meta(uint16_t idx);
void dm_blk_free(uint16_t idx);

/* Peek the next message of a class: C0 retries first, then the queue. */
bool dm_queue_head(dm_class_t c, struct dm_msg_ref *out);
/* Pop what dm_queue_head() returned. */
bool dm_queue_pop(dm_class_t c, struct dm_msg_ref *out);
/* Set refcnt and dup_pending from the DUP flags and the eligible links. */
void dm_queue_plan_copies(struct dm_msg_hdr *h);
/* Put a failed C0 message back in front of the C0 queue. */
bool dm_queue_retry_c0(uint16_t idx);
/* Requeue a failed C1 message: 1 requeued, 0 superseded, < 0 not possible. */
int dm_queue_requeue_c1(uint16_t idx);
uint32_t dm_queue_count(dm_class_t c);
bool dm_queue_any(void);
uint32_t dm_queue_bytes(dm_class_t c);

/* Consumer side lock: held by whoever pops from a class queue. Try only. */
bool dm_q_consumer_trylock(void);
void dm_q_consumer_unlock(void);

enum dm_drop_reason {
	DM_DROP_QUOTA,
	DM_DROP_SHED,
	DM_DROP_AGE,
};

/*
 * Evict from C4 towards @p floor (never C0) until @p need_bytes payload bytes
 * and @p need_blocks blocks are freed. Caller holds the consumer lock.
 * Returns the number of blocks freed.
 */
uint32_t dm_shed(uint32_t need_bytes, uint32_t need_blocks, dm_class_t floor);
/* Drop expired heads. Caller holds the consumer lock. */
uint32_t dm_queue_age_sweep(uint32_t now);

uint32_t dm_slab_used(void);
uint8_t dm_slab_pct(void);
uint32_t dm_slab_capacity_bytes(void);

/* ------------------------------------------------------------------ */
/* Links (dm_core.c, dm_owner.c)                                       */
/* ------------------------------------------------------------------ */

enum dm_disp_kind {
	DM_DISP_TX = 0,
	DM_DISP_PROBE,
};

/** Scheduler to owner thread. */
struct dm_dispatch {
	uint16_t slab_idx;
	uint16_t len;
	uint8_t cls;
	uint8_t kind;
	uint16_t handle;
	uint32_t est_us;
};

/** Owner thread to scheduler: one per dispatched tx. */
struct dm_txc {
	uint16_t slab_idx;
	uint16_t len;
	uint8_t link;
	uint8_t cls;
	uint8_t fault;
	uint8_t reserved;
	int32_t status;
	uint32_t airtime_us;
	uint32_t est_us;
};

struct dm_link {
	const struct device *dev;
	const struct dm_link_ops *ops;
	struct dm_link_caps caps;
	uint8_t index;
	int prio;
	bool started;

	/* Owner thread. */
	struct k_thread thread;
	struct k_msgq dispatch_q;
	struct dm_dispatch dispatch_buf[2];
	struct k_sem done_sem;
	atomic_t awaiting;
	int done_status;
	uint32_t done_airtime_us;
	uint32_t t0_cyc;
	uint32_t rng;
	atomic_t busy_wait_us;
	volatile uint8_t pending_irq;

	/* Scheduler owned. */
	uint8_t inflight;
	bool probe_req;
	bool probe_inflight;
	uint32_t probe_sent_tick;

	/* ISR to scheduler mailboxes. */
	atomic_t reported_state;
	atomic_t probe_seq;
	atomic_t probe_due;
	uint32_t probe_seq_seen;
	uint32_t probe_rtt_us;
	uint8_t probe_loss;
	struct k_timer probe_timer;

	/* DM_TIER_HW: completion line per class, 0 = none. */
	uint16_t hw_irqn[DM_CLASS_COUNT];
	uint8_t hw_prio[DM_CLASS_COUNT];
};

extern struct dm_link dm_links[CONFIG_DM_LINK_SLOTS];
extern uint8_t dm_n_links;

static inline uint8_t dm_registered_mask(void)
{
	return (uint8_t)BIT_MASK(dm_n_links);
}

void dm_owner_start(struct dm_link *l, k_thread_stack_t *stack, size_t stack_size);
uint8_t dm_mac_prio_for(const struct dm_link_caps *caps, dm_class_t c);
uint32_t dm_edca_backoff_us(uint32_t cw_us, uint32_t *rng);

/* ------------------------------------------------------------------ */
/* Scheduler (dm_sched.c)                                              */
/* ------------------------------------------------------------------ */

struct dm_bucket_state {
	int32_t tokens_us;
	uint32_t last_refill_tick;
	uint32_t rem; /* sub-microsecond remainder carried between refills */
};

void dm_bucket_refill(struct dm_bucket_state *b, const struct dm_bucket *p, uint32_t now);
void dm_bucket_settle(struct dm_bucket_state *b, const struct dm_bucket *p, uint32_t est_us,
		      uint32_t airtime_us);

void dm_sched_init(void);
void dm_sched_start(bool thread);
void dm_sched_stop(void);
void dm_sched_kick(void);
void dm_sched_round(uint32_t now);
void dm_sched_link_added(uint8_t l, uint32_t now);
void dm_sched_policy_changed(void);
void dm_sched_post(const struct dm_txc *t);
struct dm_bucket_state *dm_sched_bucket(dm_class_t c, uint8_t l);
uint32_t dm_sched_refill_period_ms(const struct dm_policy *p);
uint32_t dm_sched_dup_count(void);

/* ------------------------------------------------------------------ */
/* Link manager (dm_linkmgr.c)                                         */
/* ------------------------------------------------------------------ */

struct dm_lm_inputs {
	uint32_t tput_bps;
	uint32_t rtt_us;
	uint16_t loss_x10;
	uint16_t flap; /* 0..1000 */
	bool metered;
	uint8_t state;
};

uint16_t dm_lm_score_calc(const struct dm_lm_inputs *in);
uint16_t dm_lm_norm_log(uint32_t x, uint32_t full_scale);
uint16_t dm_lm_flap_decay(uint32_t elapsed_ms, uint32_t halflife_ms);
uint32_t dm_lm_dwell_ms(dm_class_t c);
bool dm_lm_switch_ok(uint16_t best, uint16_t cur, uint32_t held_ms, uint32_t dwell);
void dm_ewma_q3(uint32_t *acc, uint32_t x);

void dm_lm_init(void);
void dm_lm_link_added(uint8_t l, uint32_t now);
void dm_lm_on_tx(uint8_t l, uint16_t len, int status, uint32_t airtime_us, bool fault,
		 uint32_t now);
void dm_lm_service(uint32_t now);
bool dm_lm_serves(dm_class_t c, uint8_t l);
uint8_t dm_lm_eligible(dm_class_t c);
uint8_t dm_lm_current(dm_class_t c);
uint8_t dm_lm_alt(dm_class_t c);
bool dm_lm_dup_class(dm_class_t c);
bool dm_lm_dup_forced(void);
uint16_t dm_lm_score(uint8_t l);
enum dm_link_state dm_lm_state(uint8_t l);
void dm_lm_link_stats(uint8_t l, struct dm_link_stats *out);
void dm_lm_request_probe(uint8_t l);

/* ------------------------------------------------------------------ */
/* Policy (dm_policy.c)                                                */
/* ------------------------------------------------------------------ */

enum dm_policy_err {
	DM_POL_OK = 0,
	DM_POL_BAD_MAGIC,
	DM_POL_BAD_CRC,
	DM_POL_OLD_VERSION,
	DM_POL_UNKNOWN_LINK,
	DM_POL_EMPTY_C0,
	DM_POL_QUOTA_OVERFLOW,
	DM_POL_RATE_TOO_HIGH,
	DM_POL_BAD_THRESHOLDS,
	DM_POL_C0_NO_RATE,
};

enum dm_policy_err dm_policy_validate(const struct dm_policy *p, uint16_t cur_version,
				      uint8_t registered, uint32_t capacity);
uint32_t dm_policy_crc(const struct dm_policy *p);
void dm_policy_boot(void);
void dm_policy_service(void);
uint8_t dm_policy_mask(const struct dm_policy *p, dm_class_t c);

/** Persistent storage for the two policy slots and the boot record. */
struct dm_policy_store {
	int (*save)(const char *key, const void *data, size_t len);
	int (*load)(const char *key, void *data, size_t len);
};

/* ------------------------------------------------------------------ */
/* Pressure (dm_pressure.c)                                            */
/* ------------------------------------------------------------------ */

enum dm_pressure dm_pressure_next(enum dm_pressure cur, uint8_t pct, const uint8_t thr[2]);
dm_class_t dm_pressure_boundary(enum dm_pressure from, enum dm_pressure to);
void dm_pressure_on_submit(void);
void dm_pressure_service(void);
bool dm_pressure_refuses(dm_class_t c);
bool dm_pressure_forces_coalesce(dm_class_t c);
void dm_pressure_c0_lost(void);
void dm_pressure_reset(void);

/* ------------------------------------------------------------------ */
/* Priorities (dm_prio.c)                                              */
/* ------------------------------------------------------------------ */

/* Logical NVIC levels, in sixteenths of the priority range (section 11). */
#define DM_IRQ_HW_C0 1U
#define DM_IRQ_LINK  3U
#define DM_IRQ_HW_C4 6U
#define DM_IRQ_EDGE  10U

#if defined(CONFIG_NUM_IRQ_PRIO_BITS)
#define DM_PRIO_BITS CONFIG_NUM_IRQ_PRIO_BITS
#else
#define DM_PRIO_BITS 3 /* native_sim and other targets without an NVIC */
#endif

#if defined(_IRQ_PRIO_OFFSET)
#define DM_PRIO_OFFSET _IRQ_PRIO_OFFSET
#else
#define DM_PRIO_OFFSET 1
#endif

/*
 * Constant-expression twin of dm_irq_prio() for IRQ_CONNECT and
 * IRQ_DIRECT_CONNECT, which need compile-time priorities. Same formula:
 * ceil(level * 2^bits / 16), clamped to [1, IRQ_PRIO_LOWEST].
 */
#define DM_IRQ_PRIO_CONST(level)                                                           \
	CLAMP(DIV_ROUND_UP((level) * (1U << DM_PRIO_BITS), 16U), 1U,                       \
	      MAX((1U << DM_PRIO_BITS) - 1U - DM_PRIO_OFFSET, 1U))

unsigned int dm_irq_prio_calc(unsigned int level, unsigned int prio_bits, unsigned int offset);
unsigned int dm_irq_prio(unsigned int level);
int dm_prio_owner_default(const struct dm_link_caps *caps);

enum dm_prio_err {
	DM_PRIO_OK = 0,
	DM_PRIO_SCHED_NOT_ABOVE_OWNER,
	DM_PRIO_OWNER_COOPERATIVE,
	DM_PRIO_HW_IRQ_SHARED,
	DM_PRIO_IRQ_LEVEL_ZERO,
	DM_PRIO_SCHED_COOPERATIVE,
};

struct dm_prio_link_cfg {
	int owner_prio;
	uint8_t tier;
	uint16_t hw_irqn[DM_CLASS_COUNT];
	uint8_t hw_prio[DM_CLASS_COUNT];
};

enum dm_prio_err dm_prio_check(int sched_prio, const struct dm_prio_link_cfg *links,
			       uint8_t n);
enum dm_prio_err dm_prio_check_owner(int sched_prio, int owner_prio, uint8_t tier);
const char *dm_prio_err_str(enum dm_prio_err e);

/* ------------------------------------------------------------------ */
/* DM_TIER_HW backend (hw/)                                            */
/* ------------------------------------------------------------------ */

int dm_hw_prepare(struct dm_link *l, dm_class_t c);

/* ------------------------------------------------------------------ */
/* Counters (dm_stats.c)                                               */
/* ------------------------------------------------------------------ */

struct dm_cls_ctr {
	atomic_t submitted;
	atomic_t refused;
	atomic_t sent;
	atomic_t dropped_quota;
	atomic_t dropped_age;
	atomic_t dropped_shed;
	atomic_t coalesced;
	atomic_t failed;
	uint32_t airtime_ms;     /* scheduler only */
	uint32_t airtime_us_rem; /* scheduler only */
};

struct dm_link_ctr {
	uint32_t tx_ok; /* scheduler only, as are the next five */
	uint32_t tx_fail;
	uint32_t faults;
	uint32_t probes_ok;
	uint32_t probes_fail;
	uint32_t switches_to;
};

struct dm_ctr {
	struct dm_cls_ctr cls[DM_CLASS_COUNT];
	struct dm_link_ctr link[DM_MAX_LINKS];
	atomic_t c0_lost;
	atomic_t late_done;
	uint32_t sched_rounds;
	uint32_t sched_max_cyc;
};

extern struct dm_ctr dm_ctr;

#define DM_CTR_INC(c, field) atomic_inc(&dm_ctr.cls[(c)].field)

void dm_stats_add_airtime(dm_class_t c, uint32_t airtime_us);
void dm_stats_clear(void);

/* ------------------------------------------------------------------ */
/* Time, logging, tracing, probes (dm_core.c)                          */
/* ------------------------------------------------------------------ */

uint32_t dm_now(void);

static inline uint32_t dm_ms_to_ticks(uint32_t ms)
{
	return k_ms_to_ticks_ceil32(ms);
}

static inline uint32_t dm_ticks_to_ms(uint32_t ticks)
{
	return k_ticks_to_ms_floor32(ticks);
}

/* At most one warning per second for evictions. */
bool dm_log_ratelimit(uint32_t *last_tick);

#if defined(CONFIG_DM_TRACING)
#define DM_TRACE(name, cls, link, handle)                                                  \
	sys_trace_named_event(name, (uint32_t)(cls) | ((uint32_t)(link) << 8),             \
			      (uint32_t)(handle))
#else
#define DM_TRACE(name, cls, link, handle)                                                  \
	do {                                                                               \
	} while (false)
#endif

#if defined(CONFIG_DM_GPIO_PROBES)
void dm_probe_a_toggle(void);
void dm_probe_b_toggle(void);
#else
static inline void dm_probe_a_toggle(void)
{
}
static inline void dm_probe_b_toggle(void)
{
}
#endif

/* ------------------------------------------------------------------ */
/* Test hooks                                                          */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_DM_TEST_HOOKS)
struct dm_test_cfg {
	bool manual_sched;  /* no scheduler thread; call dm_sched_round() */
	bool keep_store;    /* keep the RAM policy store (simulated reboot) */
	bool fake_clock;    /* dm_now() returns the value set below */
	uint32_t clock;
};
void dm_test_reset(const struct dm_test_cfg *cfg);
void dm_test_clock_set(uint32_t now);
void dm_test_clock_advance_ms(uint32_t ms);
void dm_test_q_busy(bool busy);
void dm_policy_test_reset(bool keep_store);
void dm_pressure_test_reset(void);
#endif

#endif /* DOCKMASTER_DM_INTERNAL_H_ */
