/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Dockmaster counters.
 */

#ifndef DOCKMASTER_DM_STATS_H_
#define DOCKMASTER_DM_STATS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup dockmaster
 * @{
 */

/** @brief Per class counters. */
struct dm_class_stats {
	uint32_t submitted;        /**< Accepted by dm_submit(). */
	uint32_t refused;          /**< Not accepted (pressure, quota, slab, ISR -EAGAIN). */
	uint32_t sent;             /**< Delivered on at least one link. */
	uint32_t dropped_quota;    /**< Accepted, then evicted to honour the class quota. */
	uint32_t dropped_age;      /**< Dropped at head for age or deadline. */
	uint32_t dropped_shed;     /**< Evicted by the shed walk. */
	uint32_t coalesced;        /**< Replaced in place by a newer message. */
	uint32_t failed;           /**< Failed on every link it went out on (C0 retries instead). */
	uint32_t oldest_unsent_ms; /**< Age of head of queue, 0 if empty. */
	uint32_t bytes_queued;     /**< Payload bytes held in slabs. */
	uint32_t airtime_ms_used;  /**< Measured, since reset. */
};

/** @brief Per link counters. */
struct dm_link_stats {
	uint32_t tx_ok;             /**< Successful completions. */
	uint32_t tx_fail;           /**< Failed completions. */
	uint32_t faults;            /**< Watchdog faults (missing completion). */
	uint32_t probes_ok;         /**< Passed probes. */
	uint32_t probes_fail;       /**< Failed or timed out probes. */
	uint32_t switches_to;       /**< Times a class switched to this link. */
	uint32_t ewma_rtt_us;       /**< Smoothed probe round trip. */
	uint32_t ewma_tput_bps;     /**< Smoothed delivered throughput. */
	uint16_t ewma_loss_pct_x10; /**< Smoothed loss, 0..1000. */
	uint16_t score;             /**< Link manager score, 0..1000. */
	uint8_t state;              /**< enum dm_link_state. */
	uint8_t cur_tier;           /**< DM_TIER_* in use. */
	uint32_t busy_wait_ms;      /**< sw-EDCA time spent waiting, cumulative. */
};

/** @brief Snapshot of every counter. */
struct dm_stats {
	uint16_t policy_version; /**< Active policy version. */
	uint8_t pressure_state;  /**< enum dm_pressure. */
	uint8_t slab_pct;        /**< Slab occupancy, percent. */
	uint32_t c0_lost;        /**< Must be 0. */
	uint32_t sched_rounds;   /**< Scheduler rounds since reset. */
	uint32_t sched_max_us;   /**< Longest scheduler round. */
	struct dm_class_stats cls[5];  /**< Indexed by dm_class_t. */
	struct dm_link_stats link[8];  /**< Indexed by link index. */
};

/** Size of the compact encoding produced by dm_stats_encode() for @p n links. */
#define DM_STATS_ENCODED_SIZE(n) (16U + 5U * 16U + (n) * 16U)

/**
 * @brief Copy every counter. Lock-free for producers, one short irq_lock.
 */
void dm_stats_get(struct dm_stats *out);

/** @brief Zero every counter and leave the CRITICAL pressure state. */
void dm_stats_reset(void);

/**
 * @brief Encode a snapshot compactly so it fits one C1 message.
 *
 * Little endian, fixed layout: a 16 byte header, 16 bytes per class and
 * 16 bytes per registered link (four links fit in 160 bytes).
 *
 * @return Bytes written, or -ENOSPC if @p cap is too small.
 */
int dm_stats_encode(const struct dm_stats *s, uint8_t n_links, uint8_t *buf, size_t cap);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* DOCKMASTER_DM_STATS_H_ */
