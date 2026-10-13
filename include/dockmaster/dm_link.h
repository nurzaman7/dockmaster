/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Dockmaster link driver API.
 *
 * A link is anything that can transmit a message: an on-chip radio, an
 * external PHY on a bus, or a co-processor. Drivers fill in dm_link_ops and
 * dm_link_caps and call dm_link_register(). Dockmaster then owns the link:
 * exactly one owner thread calls tx, cancel and probe, and the driver reports
 * back through the ISR-safe dm_link_*_done() calls.
 */

#ifndef DOCKMASTER_DM_LINK_H_
#define DOCKMASTER_DM_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <dockmaster/dm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup dockmaster
 * @{
 */

/** @name Enforcement tiers
 * @{
 */
/** Dockmaster software queues only. */
#define DM_TIER_SW  0
/** The link exposes native MAC priority slots. */
#define DM_TIER_MAC 1
/** The SoC routes per-class completions to separate NVIC lines. */
#define DM_TIER_HW  2
/** @} */

/** Link state as reported by the driver or derived by Dockmaster. */
enum dm_link_state {
	DM_LINK_DOWN = 0, /**< Not usable. Removed from every eligible set. */
	DM_LINK_DEGRADED, /**< Usable with a lower score; C0 duplication forced. */
	DM_LINK_UP,       /**< Usable. */
};

/** @brief Static description of a link, supplied at registration. */
struct dm_link_caps {
	uint16_t mtu;             /**< Max bytes per tx call. */
	uint8_t tier;             /**< DM_TIER_*. */
	uint8_t mac_prio_levels;  /**< 0 if none, else number of native slots. */
	bool half_duplex;         /**< tx blocks rx; airtime includes rx-off time. */
	bool metered;             /**< Bytes cost money; EXCLUSIVE classes avoid it. */
	uint32_t probe_period_ms; /**< 0 = never probe periodically. */
	uint8_t owner_prio;       /**< Owner thread priority, 0 = derive from tier. */
	size_t owner_stack;       /**< Bytes, 0 = CONFIG_DM_DEFAULT_OWNER_STACK. */
};

/** @brief Driver operations. Only the owner thread calls tx, cancel, probe. */
struct dm_link_ops {
	/**
	 * Start transmitting @p len bytes. Must return quickly; completion is
	 * reported with dm_link_tx_done(), which may happen before tx returns.
	 * @p mac_prio is meaningful only when caps.tier >= DM_TIER_MAC.
	 * Return 0 and set @p handle when accepted, a negative errno otherwise.
	 */
	int (*tx)(const struct device *dev, const uint8_t *buf, size_t len,
		  uint8_t mac_prio, uint32_t *handle);
	/** Abort the transmission @p handle (watchdog path). */
	int (*cancel)(const struct device *dev, uint32_t handle);
	/** Airtime in microseconds for @p len bytes. Pure: no hardware access. */
	uint32_t (*cost_of_us)(const struct device *dev, size_t len);
	/** Optional end-to-end probe, result via dm_link_probe_done(). */
	int (*probe)(const struct device *dev);
	/** Optional: true while the medium is busy (CAD, CCA, busy pin). */
	bool (*is_busy)(const struct device *dev);
	/**
	 * Optional, DM_TIER_HW only: route the next completion to @p irqn.
	 * The owner thread calls it right before every tx whose class has a
	 * bound line (see dm_hw_bind_class_line()), and never for a class
	 * without one, so a driver can fall back to its own completion path
	 * for any tx that was not preceded by this call.
	 */
	int (*bind_class_irq)(const struct device *dev, dm_class_t cls, unsigned int irqn,
			      unsigned int prio);
};

/**
 * @brief Register a link.
 *
 * @param dev       Device passed back to every op.
 * @param ops       Driver operations; tx and cost_of_us are mandatory.
 * @param caps      Capabilities, copied.
 * @param out_index Assigned link index, used in every other call.
 *
 * @retval 0 Registered.
 * @retval -ENOSPC CONFIG_DM_LINK_SLOTS links already registered.
 * @retval -EINVAL Missing mandatory op, bad tier, or a priority that breaks
 *         the section 11 rules.
 */
int dm_link_register(const struct device *dev, const struct dm_link_ops *ops,
		     const struct dm_link_caps *caps, uint8_t *out_index);

/**
 * @brief Report the end of an accepted tx. ISR safe.
 *
 * Must be called exactly once per accepted tx. @p airtime_us of 0 with a
 * zero @p status lets Dockmaster measure the airtime itself from the moment
 * the owner thread called tx. Half-duplex links include the rx-off time.
 */
void dm_link_tx_done(uint8_t link, uint32_t handle, int status, uint32_t airtime_us);

/**
 * @brief Report a probe result. ISR safe. @p loss_pct of 100 is a failure.
 */
void dm_link_probe_done(uint8_t link, uint32_t rtt_us, uint8_t loss_pct);

/** @brief Report a link state change. ISR safe. */
void dm_link_state(uint8_t link, enum dm_link_state s);

/** @brief Number of registered links. */
uint8_t dm_link_count(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* DOCKMASTER_DM_LINK_H_ */
