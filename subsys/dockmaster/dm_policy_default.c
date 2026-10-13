/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Built-in default policy (spec section 6).
 *
 * The spec's starting quotas (1, 2, 8, 16 and 32 KiB) add up to 59 KiB,
 * more than the default 8 KiB slab, and would fail the spec's own rule that
 * quotas fit in the slab. The default therefore keeps the spec's ordering
 * (C4 largest, C0 and C1 smallest) and expresses quotas as shares of the
 * slab's payload capacity, so the default validates for any slab size.
 * Buckets are airtime, so the same values suit fast and slow links: a fast
 * link simply moves more bytes inside the same milliseconds of air.
 */

#include <string.h>

#include "dm_internal.h"

struct dm_default_row {
	uint8_t share_pct;
	uint16_t max_age_ms;
	uint8_t flags;
	uint16_t rate_ms_per_s;
	uint16_t burst_ms;
};

static const struct dm_default_row rows[DM_CLASS_COUNT] = {
	[DM_C0] = {10, 0, DM_P_DUP, 50, 200},
	[DM_C1] = {10, 5000, DM_P_COALESCE, 30, 100},
	[DM_C2] = {20, 300, 0, 200, 400},
	[DM_C3] = {25, 0, 0, 100, 1000},
	[DM_C4] = {35, 0, DM_P_EXCLUSIVE_NO_METERED, 500, 5000},
};

void dm_policy_default(struct dm_policy *out)
{
	uint32_t cap = dm_slab_capacity_bytes();

	memset(out, 0, sizeof(*out));
	out->magic = DM_POLICY_MAGIC;
	out->version = 1U;
	out->revert_window_s = 120U;
	out->depth_tighten_pct[0] = 60U;
	out->depth_tighten_pct[1] = 85U;
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		struct dm_class_policy *cp = &out->cls[c];

		cp->quota_bytes = cap * rows[c].share_pct / 100U;
		cp->max_age_ms = rows[c].max_age_ms;
		cp->link_mask = DM_LINK_MASK_ALL;
		cp->flags = rows[c].flags;
		for (int l = 0; l < DM_MAX_LINKS; l++) {
			cp->bucket[l].rate_ms_per_s = rows[c].rate_ms_per_s;
			cp->bucket[l].burst_ms = rows[c].burst_ms;
		}
	}
	dm_policy_seal(out);
}
