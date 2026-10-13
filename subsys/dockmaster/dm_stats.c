/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Counters and the stats snapshot (spec section 13).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "dm_internal.h"

struct dm_ctr dm_ctr;

void dm_stats_add_airtime(dm_class_t c, uint32_t airtime_us)
{
	struct dm_cls_ctr *cc = &dm_ctr.cls[c];
	uint32_t us = cc->airtime_us_rem + airtime_us;

	cc->airtime_ms += us / 1000U;
	cc->airtime_us_rem = us % 1000U;
}

static uint32_t head_age_ms(dm_class_t c, uint32_t now)
{
	struct dm_msg_ref head;

	if (!dm_queue_head(c, &head)) {
		return 0U;
	}
	return dm_ticks_to_ms(now - head.enq_tick);
}

void dm_stats_get(struct dm_stats *out)
{
	uint32_t now = dm_now();
	unsigned int key;

	memset(out, 0, sizeof(*out));
	key = irq_lock();
	out->policy_version = dm_policy_current()->version;
	out->pressure_state = (uint8_t)dm_pressure_get();
	out->slab_pct = dm_slab_pct();
	out->c0_lost = (uint32_t)atomic_get(&dm_ctr.c0_lost);
	out->sched_rounds = dm_ctr.sched_rounds;
	out->sched_max_us = k_cyc_to_us_ceil32(dm_ctr.sched_max_cyc);
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_cls_ctr *cc = &dm_ctr.cls[c];
		struct dm_class_stats *s = &out->cls[c];

		s->submitted = (uint32_t)atomic_get(&cc->submitted);
		s->refused = (uint32_t)atomic_get(&cc->refused);
		s->sent = (uint32_t)atomic_get(&cc->sent);
		s->dropped_quota = (uint32_t)atomic_get(&cc->dropped_quota);
		s->dropped_age = (uint32_t)atomic_get(&cc->dropped_age);
		s->dropped_shed = (uint32_t)atomic_get(&cc->dropped_shed);
		s->coalesced = (uint32_t)atomic_get(&cc->coalesced);
		s->failed = (uint32_t)atomic_get(&cc->failed);
		s->oldest_unsent_ms = head_age_ms((dm_class_t)c, now);
		s->bytes_queued = dm_queue_bytes((dm_class_t)c);
		s->airtime_ms_used = cc->airtime_ms;
	}
	for (uint8_t l = 0; l < dm_n_links; l++) {
		const struct dm_link_ctr *lc = &dm_ctr.link[l];
		struct dm_link_stats *s = &out->link[l];

		s->tx_ok = lc->tx_ok;
		s->tx_fail = lc->tx_fail;
		s->faults = lc->faults;
		s->probes_ok = lc->probes_ok;
		s->probes_fail = lc->probes_fail;
		s->switches_to = lc->switches_to;
		s->busy_wait_ms = (uint32_t)atomic_get(&dm_links[l].busy_wait_us) / 1000U;
		dm_lm_link_stats(l, s);
	}
	irq_unlock(key);
}

void dm_stats_clear(void)
{
	unsigned int key = irq_lock();

	memset(&dm_ctr, 0, sizeof(dm_ctr));
	for (uint8_t l = 0; l < dm_n_links; l++) {
		atomic_set(&dm_links[l].busy_wait_us, 0);
	}
	irq_unlock(key);
}

void dm_stats_reset(void)
{
	dm_stats_clear();
	dm_pressure_reset();
	dm_sched_kick();
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
	sys_put_le32(v, p);
	return p + 4;
}

static uint8_t *put16(uint8_t *p, uint16_t v)
{
	sys_put_le16(v, p);
	return p + 2;
}

int dm_stats_encode(const struct dm_stats *s, uint8_t n_links, uint8_t *buf, size_t cap)
{
	uint8_t *p = buf;

	n_links = MIN(n_links, DM_MAX_LINKS);
	if (cap < DM_STATS_ENCODED_SIZE(n_links)) {
		return -ENOSPC;
	}
	/* Header: 16 bytes. */
	*p++ = 1U; /* format version */
	*p++ = n_links;
	p = put16(p, s->policy_version);
	*p++ = s->pressure_state;
	*p++ = s->slab_pct;
	p = put16(p, (uint16_t)MIN(s->sched_max_us, UINT16_MAX));
	p = put32(p, s->c0_lost);
	p = put32(p, s->sched_rounds);
	/* Per class: 16 bytes. */
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_stats *k = &s->cls[c];
		uint32_t dropped = k->dropped_quota + k->dropped_age + k->dropped_shed + k->failed;

		p = put32(p, k->submitted);
		p = put32(p, k->sent);
		p = put32(p, dropped);
		p = put16(p, (uint16_t)MIN(k->oldest_unsent_ms, UINT16_MAX));
		p = put16(p, (uint16_t)MIN(k->airtime_ms_used / 1000U, UINT16_MAX));
	}
	/* Per link: 16 bytes. */
	for (uint8_t l = 0; l < n_links; l++) {
		const struct dm_link_stats *k = &s->link[l];

		p = put32(p, k->tx_ok);
		p = put16(p, (uint16_t)MIN(k->tx_fail, UINT16_MAX));
		p = put16(p, (uint16_t)MIN(k->faults, UINT16_MAX));
		p = put16(p, (uint16_t)MIN(k->ewma_rtt_us / 1000U, UINT16_MAX));
		p = put16(p, k->ewma_loss_pct_x10);
		p = put16(p, k->score);
		*p++ = k->state;
		*p++ = (uint8_t)MIN(k->switches_to, UINT8_MAX);
	}
	return (int)(p - buf);
}
