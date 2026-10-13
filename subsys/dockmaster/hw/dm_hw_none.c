/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * DM_TIER_HW stubs for SoCs without an event-routing backend. A link that
 * reports DM_TIER_HW behaves like DM_TIER_SW here.
 */

#include <errno.h>

#include <dockmaster/dm_hw.h>

#include "../dm_internal.h"

int dm_hw_bind_class_line(uint8_t link, dm_class_t cls, unsigned int egu_idx,
			  unsigned int gpiote_ch)
{
	ARG_UNUSED(link);
	ARG_UNUSED(cls);
	ARG_UNUSED(egu_idx);
	ARG_UNUSED(gpiote_ch);
	return -ENOTSUP;
}

int dm_hw_prepare(struct dm_link *l, dm_class_t c)
{
	ARG_UNUSED(l);
	ARG_UNUSED(c);
	return 0;
}
