/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interrupt and thread priority plan (spec section 11).
 *
 * dm_irq_prio() returns the priority argument for IRQ_CONNECT: Zephyr's
 * numbering, where 0 is the most urgent level a regular ISR can use. Logical
 * levels are sixteenths of the hardware range, so the same table gives
 * 1, 2, 3, 5 on 3-bit parts (nRF52) and 1, 3, 6, 10 on 4-bit parts (STM32L4).
 * Level 0 is never returned: it belongs to vendor radio stacks. With
 * CONFIG_ZERO_LATENCY_IRQS the usable range shrinks by the reserved levels.
 */

#include <zephyr/kernel.h>

#include "dm_internal.h"

#if defined(CONFIG_NUM_IRQ_PRIO_BITS)
#define DM_PRIO_BITS CONFIG_NUM_IRQ_PRIO_BITS
#else
#define DM_PRIO_BITS 3 /* native_sim and other targets without an NVIC */
#endif

#if defined(_IRQ_PRIO_OFFSET)
#define DM_PRIO_OFFSET _IRQ_PRIO_OFFSET
#else
#define DM_PRIO_OFFSET 1
#endif

unsigned int dm_irq_prio_calc(unsigned int level, unsigned int prio_bits, unsigned int offset)
{
	unsigned int n = 1U << prio_bits;
	unsigned int lowest = (n > offset + 1U) ? n - 1U - offset : 1U;
	unsigned int p = DIV_ROUND_UP(level * n, 16U);

	return CLAMP(p, 1U, MAX(lowest, 1U));
}

unsigned int dm_irq_prio(unsigned int level)
{
	return dm_irq_prio_calc(level, DM_PRIO_BITS, DM_PRIO_OFFSET);
}

/* Section 11 thread table: MAC links sched+1, SW sched+2, metered sched+3. */
int dm_prio_owner_default(const struct dm_link_caps *caps)
{
	if (caps->metered) {
		return CONFIG_DM_SCHED_PRIO + 3;
	}
	return CONFIG_DM_SCHED_PRIO + (caps->tier >= DM_TIER_MAC ? 1 : 2);
}

enum dm_prio_err dm_prio_check_owner(int sched_prio, int owner_prio, uint8_t tier)
{
	if (owner_prio < 0) {
		return DM_PRIO_OWNER_COOPERATIVE;
	}
	/* Only a MAC-on-core link may run its owner above the scheduler. */
	if (owner_prio <= sched_prio && tier != DM_TIER_MAC) {
		return DM_PRIO_SCHED_NOT_ABOVE_OWNER;
	}
	return DM_PRIO_OK;
}

enum dm_prio_err dm_prio_check(int sched_prio, const struct dm_prio_link_cfg *links, uint8_t n)
{
	if (sched_prio < 0) {
		return DM_PRIO_SCHED_COOPERATIVE;
	}
	for (uint8_t i = 0; i < n; i++) {
		const struct dm_prio_link_cfg *a = &links[i];
		enum dm_prio_err e = dm_prio_check_owner(sched_prio, a->owner_prio, a->tier);

		if (e != DM_PRIO_OK) {
			return e;
		}
		for (int c = 0; c < DM_CLASS_COUNT; c++) {
			if (a->hw_irqn[c] == 0U) {
				continue;
			}
			if (a->hw_prio[c] == 0U) {
				return DM_PRIO_IRQ_LEVEL_ZERO;
			}
			/* Classes sharing a line must share a level (C0-C2, C3-C4). */
			for (uint8_t j = 0; j < n; j++) {
				for (int d = 0; d < DM_CLASS_COUNT; d++) {
					const struct dm_prio_link_cfg *b = &links[j];
					bool same = (i == j && c == d);

					if (!same && b->hw_irqn[d] == a->hw_irqn[c] &&
					    b->hw_prio[d] != a->hw_prio[c]) {
						return DM_PRIO_HW_IRQ_SHARED;
					}
					if (i != j && b->hw_irqn[d] == a->hw_irqn[c]) {
						return DM_PRIO_HW_IRQ_SHARED;
					}
				}
			}
		}
	}
	return DM_PRIO_OK;
}

const char *dm_prio_err_str(enum dm_prio_err e)
{
	switch (e) {
	case DM_PRIO_OK:
		return "ok";
	case DM_PRIO_SCHED_NOT_ABOVE_OWNER:
		return "scheduler priority not above an owner thread";
	case DM_PRIO_OWNER_COOPERATIVE:
		return "owner thread is cooperative";
	case DM_PRIO_HW_IRQ_SHARED:
		return "DM_TIER_HW class line IRQn shared";
	case DM_PRIO_IRQ_LEVEL_ZERO:
		return "Dockmaster IRQ at level 0";
	case DM_PRIO_SCHED_COOPERATIVE:
		return "scheduler thread is cooperative";
	}
	return "?";
}
