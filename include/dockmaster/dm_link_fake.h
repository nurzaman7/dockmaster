/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Fake link for tests and host demos.
 *
 * A pure software link with an airtime model, configurable completion
 * latency, loss, busy medium, missing completions and probes. Completions
 * arrive from a k_timer, i.e. from interrupt context, like a real radio.
 */

#ifndef DOCKMASTER_DM_LINK_FAKE_H_
#define DOCKMASTER_DM_LINK_FAKE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <dockmaster/dm_link.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fake link behaviour. */
struct dm_fake_cfg {
	struct dm_link_caps caps;   /**< Passed to dm_link_register(). */
	uint32_t bitrate_bps;       /**< Airtime model: len * 8 / bitrate. */
	uint32_t overhead_us;       /**< Fixed airtime per frame (preamble, guard). */
	uint32_t latency_us;        /**< Completion delay; 0 completes inside tx. */
	bool airtime_latency;       /**< Completion delay equals the modelled airtime. */
	uint32_t airtime_pct;       /**< Reported airtime as % of the estimate, 0 = 100. */
	uint32_t rx_turnaround_us;  /**< Added to airtime on half-duplex links. */
	uint8_t loss_pct;           /**< Completions failing with -EIO. */
	bool drop_completion;       /**< Never complete: exercises the watchdog. */
	bool down;                  /**< tx returns -ENETDOWN. */
	uint32_t probe_rtt_us;      /**< Probe result RTT. */
	bool probe_fail;            /**< Probes report 100 % loss. */
	uint32_t busy_checks;       /**< The next N is_busy() calls return true. */
	uint32_t seed;              /**< PRNG seed for loss. */
};

/** @brief Counters kept by a fake link. */
struct dm_fake_counters {
	uint32_t tx_calls;    /**< Calls to tx, accepted or not. */
	uint32_t tx_ok;       /**< Completions reported with status 0. */
	uint32_t tx_fail;     /**< Completions reported with an error. */
	uint32_t bytes_ok;    /**< Payload bytes completed successfully. */
	uint32_t cancels;     /**< Calls to cancel (watchdog path). */
	uint32_t probes;      /**< Calls to probe. */
	uint32_t busy_checks; /**< Calls to is_busy. */
	uint32_t overlap;     /**< tx called while a previous tx was outstanding. */
	uint8_t last_mac_prio; /**< mac_prio of the most recent tx. */
};

/**
 * @brief Called on every completion with the payload, from the completion
 * context (timer ISR, or the owner thread when latency is 0).
 */
typedef void (*dm_fake_tx_cb_t)(uint8_t fake, const uint8_t *buf, size_t len, int status);

/** @brief Fake instance @p i (0 .. CONFIG_DM_LINK_FAKE_COUNT - 1). */
const struct device *dm_fake_get(uint8_t i);

/** @brief Set behaviour; may be called again at run time. */
void dm_fake_configure(uint8_t i, const struct dm_fake_cfg *cfg);

/** @brief Live access to the configuration (tests flip fields at run time). */
struct dm_fake_cfg *dm_fake_cfg(uint8_t i);

/** @brief Register instance @p i with Dockmaster using its caps. */
int dm_fake_register(uint8_t i, uint8_t *out_link);

/** @brief Install the completion callback for every instance. */
void dm_fake_set_tx_cb(dm_fake_tx_cb_t cb);

/** @brief Copy the counters of instance @p i. */
void dm_fake_counters(uint8_t i, struct dm_fake_counters *out);

/** @brief Stop timers and zero counters and configuration of every instance. */
void dm_fake_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DOCKMASTER_DM_LINK_FAKE_H_ */
