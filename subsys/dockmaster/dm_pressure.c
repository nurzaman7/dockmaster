/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backpressure (spec section 12): one state machine driven by slab
 * occupancy, with 10 points of hysteresis below each threshold.
 * Producers read the state to refuse classes; transitions, logging and the
 * producer hint happen on the scheduler thread only.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

#define HYST_PCT 10U

static atomic_t state;
static enum dm_pressure reported;
static dm_hint_cb_t hint;

static const char *const names[] = {"NORMAL", "TIGHT", "SHED", "CRITICAL"};

enum dm_pressure dm_pressure_next(enum dm_pressure cur, uint8_t pct, const uint8_t thr[2])
{
	if (cur == DM_PRESSURE_CRITICAL) {
		return cur; /* sticky until dm_stats_reset() */
	}
	if (pct >= thr[1]) {
		return DM_PRESSURE_SHED;
	}
	if (cur == DM_PRESSURE_SHED && pct >= thr[1] - HYST_PCT) {
		return DM_PRESSURE_SHED;
	}
	if (pct >= thr[0]) {
		return DM_PRESSURE_TIGHT;
	}
	if (cur >= DM_PRESSURE_TIGHT && pct >= thr[0] - HYST_PCT) {
		return DM_PRESSURE_TIGHT;
	}
	return DM_PRESSURE_NORMAL;
}

/* The producer group whose boundary a transition crossed (most severe). */
dm_class_t dm_pressure_boundary(enum dm_pressure from, enum dm_pressure to)
{
	if (to == DM_PRESSURE_CRITICAL) {
		return DM_C0;
	}
	if ((from >= DM_PRESSURE_SHED) != (to >= DM_PRESSURE_SHED)) {
		return DM_C3;
	}
	return DM_C2;
}

enum dm_pressure dm_pressure_get(void)
{
	return (enum dm_pressure)atomic_get(&state);
}

void dm_hint_register(dm_hint_cb_t cb)
{
	hint = cb;
}

bool dm_pressure_refuses(dm_class_t c)
{
	enum dm_pressure s = dm_pressure_get();

	return (c == DM_C4 && s >= DM_PRESSURE_TIGHT) || (c == DM_C3 && s >= DM_PRESSURE_SHED);
}

bool dm_pressure_forces_coalesce(dm_class_t c)
{
	return c == DM_C1 && dm_pressure_get() >= DM_PRESSURE_SHED;
}

/* Cheap check after each submit: wake the scheduler only on a band change. */
void dm_pressure_on_submit(void)
{
	enum dm_pressure cur = dm_pressure_get();

	if (dm_pressure_next(cur, dm_slab_pct(), dm_policy_current()->depth_tighten_pct) != cur) {
		dm_sched_kick();
	}
}

void dm_pressure_c0_lost(void)
{
	atomic_inc(&dm_ctr.c0_lost);
	atomic_set(&state, DM_PRESSURE_CRITICAL);
	LOG_ERR("C0 message lost");
	dm_sched_kick();
	if (IS_ENABLED(CONFIG_DM_C0_LOSS_FATAL)) {
		k_oops();
	}
}

void dm_pressure_service(void)
{
	enum dm_pressure cur = dm_pressure_get();
	enum dm_pressure next =
		dm_pressure_next(cur, dm_slab_pct(), dm_policy_current()->depth_tighten_pct);

	if (next != cur) {
		atomic_set(&state, next);
	}
	if (next == reported) {
		return;
	}
	if (next == DM_PRESSURE_CRITICAL) {
		LOG_ERR("pressure %s -> %s", names[reported], names[next]);
	} else {
		LOG_WRN("pressure %s -> %s", names[reported], names[next]);
	}
	if (hint != NULL) {
		hint(dm_pressure_boundary(reported, next), next);
	}
	reported = next;
}

void dm_pressure_reset(void)
{
	atomic_set(&state, DM_PRESSURE_NORMAL);
	reported = DM_PRESSURE_NORMAL;
}

#if defined(CONFIG_DM_TEST_HOOKS)
/* Lets the pressure suite start from a clean slate without a full reset. */
void dm_pressure_test_reset(void)
{
	dm_pressure_reset();
	hint = NULL;
}
#endif
