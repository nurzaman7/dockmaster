/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Helpers shared by the Dockmaster ztest suites. The suites run with the
 * scheduler in manual mode and a fake clock, so every token value and every
 * transition is deterministic. Owner threads run at priority 5..7, above the
 * test thread (10), so a dispatch is transmitted before dm_sched_round()
 * returns; with the fake link's zero latency the completion is then settled
 * by the next round.
 */

#ifndef DM_TEST_COMMON_H_
#define DM_TEST_COMMON_H_

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <dockmaster/dm.h>
#include <dockmaster/dm_link_fake.h>

#include "dm_internal.h"

#define DMT_T0      1000U
#define DMT_LOG_MAX 512

struct dmt_tx {
	uint8_t fake;
	uint8_t cls;
	uint32_t seq;
	uint16_t len;
	int status;
};

static struct dmt_tx dmt_log[DMT_LOG_MAX];
static volatile int dmt_log_n;

static inline void dmt_tx_cb(uint8_t fake, const uint8_t *buf, size_t len, int status)
{
	if (dmt_log_n < DMT_LOG_MAX) {
		dmt_log[dmt_log_n] = (struct dmt_tx){
			.fake = fake,
			.cls = buf[0],
			.seq = sys_get_le32(&buf[1]),
			.len = (uint16_t)len,
			.status = status,
		};
		dmt_log_n++;
	}
}

/* 1 Mbit/s, 8 us of air per byte, no overhead, completes inside tx. */
static inline struct dm_fake_cfg dmt_link_cfg(bool metered)
{
	return (struct dm_fake_cfg){
		.caps = {.mtu = 255, .tier = DM_TIER_MAC, .mac_prio_levels = 2,
			 .metered = metered},
		.bitrate_bps = 1000000,
	};
}

static inline void dmt_reset_with(bool keep_store)
{
	struct dm_test_cfg cfg = {
		.manual_sched = true,
		.fake_clock = true,
		.clock = DMT_T0,
		.keep_store = keep_store,
	};

	dm_fake_reset_all();
	dm_test_reset(&cfg);
	dm_fake_set_tx_cb(dmt_tx_cb);
	dmt_log_n = 0;
}

static inline void dmt_reset(void)
{
	dmt_reset_with(false);
}

static inline uint8_t dmt_add_link(uint8_t fake, const struct dm_fake_cfg *cfg)
{
	uint8_t link = 0xFF;

	dm_fake_configure(fake, cfg);
	zassert_ok(dm_fake_register(fake, &link), "fake %u did not register", fake);
	return link;
}

static inline void dmt_round(void)
{
	dm_sched_round(dm_now());
}

/* Run rounds until nothing is queued or in flight (bounded). */
static inline void dmt_drain(int max_rounds)
{
	for (int i = 0; i < max_rounds; i++) {
		dmt_round();
		if (!dm_queue_any() && dm_slab_used() == dm_sched_dup_count()) {
			bool busy = false;

			for (uint8_t l = 0; l < dm_n_links; l++) {
				busy = busy || dm_links[l].inflight != 0U;
			}
			if (!busy) {
				dmt_round();
				return;
			}
		}
	}
}

static inline int dmt_submit(dm_class_t c, uint32_t seq, size_t len, uint32_t flags,
			     const struct dm_submit_opts *o)
{
	uint8_t buf[DM_MAX_MSG];

	memset(buf, 0xC5, len);
	buf[0] = (uint8_t)c;
	if (len >= 5U) {
		sys_put_le32(seq, &buf[1]);
	}
	return dm_submit(c, buf, len, flags, o);
}

/* Default policy at version @p v; edit it, then dm_policy_seal() it. */
static inline void dmt_policy(struct dm_policy *p, uint16_t v)
{
	dm_policy_default(p);
	p->version = v;
	dm_policy_seal(p);
}

static inline uint32_t dmt_count(uint8_t fake, uint8_t cls)
{
	uint32_t n = 0;

	for (int i = 0; i < dmt_log_n; i++) {
		n += (dmt_log[i].fake == fake && dmt_log[i].cls == cls) ? 1U : 0U;
	}
	return n;
}

#endif /* DM_TEST_COMMON_H_ */
