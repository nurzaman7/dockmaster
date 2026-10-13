/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief DM_TIER_HW backend API (spec section 11, "tier 0").
 */

#ifndef DOCKMASTER_DM_HW_H_
#define DOCKMASTER_DM_HW_H_

#include <stdint.h>

#include <dockmaster/dm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup dockmaster
 * @{
 */

/**
 * @brief Route one class's completion line to its own NVIC interrupt.
 *
 * Drivers call this once per class they can separate. Dockmaster picks the
 * NVIC level from the section 11 table: DM_IRQ_HW_C0 for C0 to C2,
 * DM_IRQ_HW_C4 for C3 and C4. On nRF52/nRF53 the line is
 * GPIOTE IN[@p gpiote_ch] -> PPI/DPPI -> EGU[@p egu_idx].TRIGGER[0].
 *
 * @retval 0 Bound.
 * @retval -ENOTSUP No DM_TIER_HW backend on this SoC.
 * @retval -EINVAL Bad link, class, EGU or channel.
 */
int dm_hw_bind_class_line(uint8_t link, dm_class_t cls, unsigned int egu_idx,
			  unsigned int gpiote_ch);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* DOCKMASTER_DM_HW_H_ */
