/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Owner threads (spec section 9). Each link has exactly one: the only thread
 * that calls its tx, cancel and probe. It blocks only on its own link's
 * completion, guarded by a watchdog of four times the estimated airtime.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

#if defined(CONFIG_DM_EDCA)
static const uint32_t aifs_us[DM_CLASS_COUNT] = {
	CONFIG_DM_EDCA_AIFS_US_C0, CONFIG_DM_EDCA_AIFS_US_C1, CONFIG_DM_EDCA_AIFS_US_C2,
	CONFIG_DM_EDCA_AIFS_US_C3, CONFIG_DM_EDCA_AIFS_US_C4,
};
static const uint32_t cw_us[DM_CLASS_COUNT] = {
	CONFIG_DM_EDCA_CW_US_C0, CONFIG_DM_EDCA_CW_US_C1, CONFIG_DM_EDCA_CW_US_C2,
	CONFIG_DM_EDCA_CW_US_C3, CONFIG_DM_EDCA_CW_US_C4,
};
#endif

/*
 * Class to native MAC priority. With four levels C2 (media) ranks above C1
 * (state) on the medium, matching Wi-Fi VI above BE; stale state is
 * coalesced anyway.
 */
uint8_t dm_mac_prio_for(const struct dm_link_caps *caps, dm_class_t c)
{
	static const uint8_t map4[DM_CLASS_COUNT] = {0, 2, 1, 2, 3};
	static const uint8_t map3[DM_CLASS_COUNT] = {0, 1, 1, 2, 2};

	if (caps->tier < DM_TIER_MAC || caps->mac_prio_levels <= 1U) {
		return 0U;
	}
	if (caps->mac_prio_levels == 2U) {
		return c <= DM_C2 ? 0U : 1U;
	}
	if (caps->mac_prio_levels == 3U) {
		return map3[c];
	}
	return map4[c];
}

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s != 0U ? *s : 0x2545F491U;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* Uniform in [0, cw_us]. */
uint32_t dm_edca_backoff_us(uint32_t cw, uint32_t *rng)
{
	return cw == 0U ? 0U : xorshift32(rng) % (cw + 1U);
}

#if defined(CONFIG_DM_EDCA)
/*
 * Software EDCA: wait AIFS plus a random slice of the contention window,
 * then re-check the medium. Busy doubles the window (up to four times the
 * initial one) and tries again, up to CONFIG_DM_EDCA_RETRIES times.
 */
static int sw_edca_wait(struct dm_link *l, dm_class_t c)
{
	uint32_t cw = cw_us[c];
	uint32_t cw_max = cw_us[c] * 4U;

	for (int attempt = 0;; attempt++) {
		uint32_t wait = aifs_us[c] + dm_edca_backoff_us(cw, &l->rng);

		k_sleep(K_USEC(wait));
		atomic_add(&l->busy_wait_us, (atomic_val_t)wait);
		if (!l->ops->is_busy(l->dev)) {
			return 0;
		}
		if (attempt >= CONFIG_DM_EDCA_RETRIES) {
			return -EBUSY;
		}
		cw = MIN(cw * 2U, cw_max);
	}
}
#endif

static void owner_tx(struct dm_link *l, const struct dm_dispatch *d)
{
	struct dm_txc t = {
		.slab_idx = d->slab_idx,
		.len = d->len,
		.link = l->index,
		.cls = d->cls,
		.est_us = d->est_us,
	};
	int rc = 0;

#if defined(CONFIG_DM_EDCA)
	if (l->caps.tier == DM_TIER_SW && l->ops->is_busy != NULL) {
		rc = sw_edca_wait(l, (dm_class_t)d->cls);
	}
#endif
	if (rc == 0 && l->caps.tier == DM_TIER_HW) {
		rc = dm_hw_prepare(l, (dm_class_t)d->cls);
	}

	if (rc == 0) {
		uint32_t handle = 0U;

		k_sem_reset(&l->done_sem);
		atomic_set(&l->awaiting, 1);
		l->t0_cyc = k_cycle_get_32();
		dm_probe_b_toggle();
		DM_TRACE("dm_owner_tx", d->cls, l->index, d->handle);
		rc = l->ops->tx(l->dev, dm_blk_payload(d->slab_idx), d->len,
				dm_mac_prio_for(&l->caps, (dm_class_t)d->cls), &handle);
		if (rc != 0) {
			atomic_set(&l->awaiting, 0);
		} else {
			uint32_t wd_us = MAX(4U * d->est_us, DM_WATCHDOG_MIN_US);

			if (k_sem_take(&l->done_sem, K_USEC(wd_us)) != 0) {
				if (atomic_cas(&l->awaiting, 1, 0)) {
					/* Missing completion: cancel and count a fault. */
					if (l->ops->cancel != NULL) {
						(void)l->ops->cancel(l->dev, handle);
					}
					t.fault = 1U;
					rc = -ETIMEDOUT;
				} else {
					/* Completion raced the watchdog; it is imminent. */
					(void)k_sem_take(&l->done_sem, K_FOREVER);
				}
			}
			if (t.fault == 0U) {
				rc = l->done_status;
				t.airtime_us = l->done_airtime_us;
			}
		}
	}
	t.status = rc;
	dm_sched_post(&t);
}

static void owner_loop(void *p1, void *p2, void *p3)
{
	struct dm_link *l = p1;
	struct dm_dispatch d;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)k_msgq_get(&l->dispatch_q, &d, K_FOREVER);
		if (d.kind == DM_DISP_PROBE) {
			int rc = l->ops->probe != NULL ? l->ops->probe(l->dev) : -ENOTSUP;

			if (rc != 0) {
				dm_link_probe_done(l->index, 0U, 100U);
			}
			continue;
		}
		owner_tx(l, &d);
	}
}

void dm_owner_start(struct dm_link *l, k_thread_stack_t *stack, size_t stack_size)
{
	size_t sz = l->caps.owner_stack != 0U ? MIN(l->caps.owner_stack, stack_size) : stack_size;

	l->rng = 0x9E3779B9U ^ ((uint32_t)l->index << 16) ^ k_cycle_get_32();
	k_thread_create(&l->thread, stack, sz, owner_loop, l, NULL, NULL, l->prio, 0, K_NO_WAIT);
	k_thread_name_set(&l->thread, "dm_owner");
	if (l->caps.probe_period_ms != 0U && l->ops->probe != NULL) {
		k_timer_start(&l->probe_timer, K_MSEC(l->caps.probe_period_ms),
			      K_MSEC(l->caps.probe_period_ms));
	}
	l->started = true;
}

/* ------------------------------------------------------------------ */
/* Driver to Dockmaster, ISR safe                                      */
/* ------------------------------------------------------------------ */

void dm_link_tx_done(uint8_t link, uint32_t handle, int status, uint32_t airtime_us)
{
	struct dm_link *l;

	if (link >= dm_n_links) {
		return;
	}
	l = &dm_links[link];
	if (k_is_in_isr()) {
		dm_probe_a_toggle();
	}
	if (!atomic_cas(&l->awaiting, 1, 0)) {
		/* Late (after the watchdog) or duplicate completion. */
		atomic_inc(&dm_ctr.late_done);
		return;
	}
	if (status == 0 && airtime_us == 0U) {
		airtime_us = k_cyc_to_us_floor32(k_cycle_get_32() - l->t0_cyc);
	}
	l->done_status = status;
	l->done_airtime_us = airtime_us;
	DM_TRACE("dm_tx_done", DM_CLASS_COUNT, link, handle);
	k_sem_give(&l->done_sem);
}

void dm_link_probe_done(uint8_t link, uint32_t rtt_us, uint8_t loss_pct)
{
	struct dm_link *l;

	if (link >= dm_n_links) {
		return;
	}
	l = &dm_links[link];
	l->probe_rtt_us = rtt_us;
	l->probe_loss = MIN(loss_pct, 100U);
	atomic_inc(&l->probe_seq);
	dm_sched_kick();
}

void dm_link_state(uint8_t link, enum dm_link_state s)
{
	if (link >= dm_n_links) {
		return;
	}
	atomic_set(&dm_links[link].reported_state, (atomic_val_t)s);
	dm_sched_kick();
}
