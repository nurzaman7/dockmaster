/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Dockmaster producer and policy API.
 *
 * Dockmaster gives a multi-radio Cortex-M device one traffic-class table, one
 * airtime scheduler and one link selector. Producers call dm_submit() with a
 * class; everything after that (queueing, budgeting, link choice, shedding) is
 * decided by the policy and never blocks the producer.
 */

#ifndef DOCKMASTER_DM_H_
#define DOCKMASTER_DM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup dockmaster Dockmaster
 * @brief Firmware QoS for multi-radio Cortex-M devices.
 * @{
 */

/** @brief Traffic class. Lower number wins everywhere. */
typedef enum {
	DM_C0 = 0,     /**< Control. Never shed, may overdraw its bucket. */
	DM_C1,         /**< State. Coalesced, freshest wins. */
	DM_C2,         /**< Media. Deadline driven. */
	DM_C3,         /**< Events. */
	DM_C4,         /**< Bulk. First to be shed. */
	DM_CLASS_COUNT /**< Number of classes. */
} dm_class_t;

/** @name Submit flags
 * @{
 */
/** Replace an older queued message with the same coalesce key (C1). */
#define DM_F_COALESCE  BIT(0)
/** Discard the message if its deadline passes before transmission (C2). */
#define DM_F_DROP_LATE BIT(1)
/** The driver may chunk and resume this message (C3, C4). */
#define DM_F_RESUMABLE BIT(2)
/** Send on two links if affinity allows (C0). */
#define DM_F_DUP       BIT(3)
/** @} */

/** @name Policy flags (dm_class_policy::flags)
 * @{
 */
/** Every message of the class is duplicated on the two best eligible links. */
#define DM_P_DUP                  BIT(0)
/** Metered links are removed from the eligible set regardless of score. */
#define DM_P_EXCLUSIVE_NO_METERED BIT(1)
/** Every message of the class is coalesced by its key. */
#define DM_P_COALESCE             BIT(2)
/** @} */

/** Link hint meaning "any link the link manager prefers". */
#define DM_LINK_ANY      0xFFU
/** Maximum number of links a policy can describe. */
#define DM_MAX_LINKS     8
/** link_mask value meaning "every registered link", whatever registers later. */
#define DM_LINK_MASK_ALL 0xFFU

/** Policy magic, "DMP1". */
#define DM_POLICY_MAGIC 0x444D5031U

/** Largest payload dm_submit() accepts, in bytes. */
#define DM_MAX_MSG CONFIG_DM_MAX_MSG

/** @brief Per-submit options. All fields may be left zero. */
struct dm_submit_opts {
	/** Deadline relative to now in ms, 0 for none. Used with DM_F_DROP_LATE. */
	uint32_t deadline_ms;
	/** Coalesce key. Used with DM_F_COALESCE or when coalescing is forced. */
	uint32_t coalesce_key;
	/** DM_LINK_ANY or a registered link index. */
	uint8_t link_hint;
};

/** @brief Token bucket, in milliseconds of airtime. */
struct dm_bucket {
	uint16_t rate_ms_per_s; /**< Sustained budget, ms of air per second. */
	uint16_t burst_ms;      /**< Bucket depth in ms of air. */
} __packed;

/** @brief Policy for one class. */
struct dm_class_policy {
	uint32_t quota_bytes; /**< Max payload bytes the class may hold in slabs. */
	uint16_t max_age_ms;  /**< 0 = no age limit, else drop at head when exceeded. */
	uint8_t link_mask;    /**< Link indices that may carry the class. */
	uint8_t flags;        /**< DM_P_* flags. */
	struct dm_bucket bucket[DM_MAX_LINKS]; /**< One bucket per link index. */
} __packed;

/** @brief Versioned, CRC protected policy artifact. */
struct dm_policy {
	uint32_t magic;               /**< DM_POLICY_MAGIC. */
	uint16_t version;             /**< Applied only if greater than current. */
	uint16_t revert_window_s;     /**< Two reboots inside this window revert. */
	uint8_t depth_tighten_pct[2]; /**< Slab occupancy thresholds, e.g. {60, 85}. */
	struct dm_class_policy cls[DM_CLASS_COUNT]; /**< Per class policy. */
	uint32_t crc32;               /**< CRC-32 (IEEE) over everything above. */
} __packed;

/** @brief Pressure state, driven by slab occupancy. */
enum dm_pressure {
	DM_PRESSURE_NORMAL = 0, /**< No restrictions. */
	DM_PRESSURE_TIGHT,      /**< C4 refused, C2 producers asked to slow down. */
	DM_PRESSURE_SHED,       /**< C3 refused too, proactive shed, C1 coalesce forced. */
	DM_PRESSURE_CRITICAL,   /**< A C0 message was lost. Sticky until stats reset. */
};

/**
 * @brief Producer hint callback.
 *
 * Called from the scheduler thread, never from an ISR, exactly once per
 * pressure transition. @p cls names the producer group whose boundary was
 * crossed: DM_C2 for the TIGHT boundary (rate hint), DM_C3 for the SHED
 * boundary (refusal), DM_C0 for CRITICAL. @p p is the new state.
 */
typedef void (*dm_hint_cb_t)(dm_class_t cls, enum dm_pressure p);

/**
 * @brief Copy a message into a slab block and enqueue it.
 *
 * O(1) on the fast path, never blocks and never allocates from the heap.
 * Callable from an ISR; in that case coalescing and shedding (both
 * O(queue depth)) are skipped and -EAGAIN is returned instead so the caller
 * can defer to a thread.
 *
 * @param cls   Traffic class.
 * @param buf   Payload.
 * @param len   Payload length, 1..DM_MAX_MSG.
 * @param flags DM_F_* flags.
 * @param opts  Options, may be NULL.
 *
 * @retval 0 Enqueued.
 * @retval -ENOMEM Slab empty after one shed, caller may retry later.
 * @retval -ENOSPC Class quota full and nothing lower to evict, or the class is
 *         refused at the current pressure level.
 * @retval -EINVAL Bad class, length or link hint.
 * @retval -EAGAIN ISR context and the request needs coalesce or shed, or a
 *         thread holds the queue for a shed walk.
 */
int dm_submit(dm_class_t cls, const void *buf, size_t len, uint32_t flags,
	      const struct dm_submit_opts *opts);

/**
 * @brief Allocate a slab block for zero-copy submission.
 *
 * @param len     Payload length the caller intends to write.
 * @param timeout How long to wait for a free block.
 * @return Pointer to DM_MAX_MSG bytes of payload space, or NULL.
 */
void *dm_alloc(size_t len, k_timeout_t timeout);

/**
 * @brief Return a block from dm_alloc() without submitting it.
 *
 * @param block Pointer returned by dm_alloc().
 */
void dm_free(void *block);

/**
 * @brief Enqueue a block obtained from dm_alloc().
 *
 * Ownership passes to Dockmaster whatever the return value.
 *
 * @return Same codes as dm_submit().
 */
int dm_submit_block(dm_class_t cls, void *block, size_t len, uint32_t flags,
		    const struct dm_submit_opts *opts);

/**
 * @brief Validate a policy and swap it in atomically.
 *
 * Checks magic, CRC, version, link masks, quotas and rates; on any failure
 * nothing changes. The previous policy is kept for dm_policy_revert() and,
 * when the settings subsystem is enabled, both are persisted.
 *
 * @retval 0 Applied.
 * @retval -EINVAL Validation failed.
 */
int dm_policy_apply(const struct dm_policy *p);

/**
 * @brief Go back to the previous policy.
 *
 * @retval 0 Reverted.
 * @retval -ENOENT No previous policy.
 */
int dm_policy_revert(void);

/** @brief Active policy. Do not hold the pointer across a blocking call. */
const struct dm_policy *dm_policy_current(void);

/**
 * @brief Fill @p out with the built-in default policy (version 1, sealed).
 */
void dm_policy_default(struct dm_policy *out);

/** @brief Recompute the CRC of @p p after editing it. */
void dm_policy_seal(struct dm_policy *p);

/** @brief Register the producer hint callback, NULL to remove it. */
void dm_hint_register(dm_hint_cb_t cb);

/** @brief Current pressure state. */
enum dm_pressure dm_pressure_get(void);

/**
 * @brief Initialise Dockmaster.
 *
 * Runs automatically as a SYS_INIT at APPLICATION level. Creates the scheduler
 * thread and one owner thread per link registered so far; links registered
 * later get their owner thread at registration.
 *
 * @retval 0 Running.
 * @retval -EINVAL An init-time priority assertion failed (logged).
 */
int dm_init(void);

/** @} */

#ifdef __cplusplus
}
#endif

#include <dockmaster/dm_stats.h>

#endif /* DOCKMASTER_DM_H_ */
