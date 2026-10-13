/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interrupt and priority plan (spec section 11), table driven.
 */

#include "dm_test_common.h"

static void before(void *f)
{
	ARG_UNUSED(f);
	dmt_reset();
}

ZTEST_SUITE(prio, NULL, NULL, before, NULL, NULL);

ZTEST(prio, test_irq_prio_table)
{
	static const struct {
		uint8_t bits, offset;
		uint8_t hw_c0, link, hw_c4, edge;
	} t[] = {
		{2, 1, 1, 1, 2, 2},       /* 4 levels: everything squeezed */
		{3, 1, 1, 2, 3, 5},       /* nRF52 */
		{4, 1, 1, 3, 6, 10},      /* STM32L4 */
		{8, 1, 16, 48, 96, 160},  /* 8 bits */
		{3, 2, 1, 2, 3, 5},       /* zero-latency IRQs reserve one more level */
		{4, 2, 1, 3, 6, 10},
		{2, 2, 1, 1, 1, 1},
	};

	for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
		unsigned int b = t[i].bits, o = t[i].offset;
		unsigned int lowest = (1U << b) - 1U - o;

		zassert_equal(dm_irq_prio_calc(DM_IRQ_HW_C0, b, o), t[i].hw_c0, "row %u", i);
		zassert_equal(dm_irq_prio_calc(DM_IRQ_LINK, b, o), t[i].link, "row %u", i);
		zassert_equal(dm_irq_prio_calc(DM_IRQ_HW_C4, b, o), t[i].hw_c4, "row %u", i);
		zassert_equal(dm_irq_prio_calc(DM_IRQ_EDGE, b, o), t[i].edge, "row %u", i);
		for (unsigned int lvl = 0; lvl <= 16U; lvl++) {
			unsigned int p = dm_irq_prio_calc(lvl, b, o);

			zassert_true(p >= 1U, "level 0 is never used (row %u)", i);
			zassert_true(p <= MAX(lowest, 1U), "above IRQ_PRIO_LOWEST (row %u)", i);
		}
	}
	zassert_true(dm_irq_prio(DM_IRQ_HW_C0) >= 1U);
	BUILD_ASSERT(DM_IRQ_PRIO_CONST(DM_IRQ_HW_C0) >= 1U, "usable in constant expressions");
	zassert_equal(DM_IRQ_PRIO_CONST(DM_IRQ_HW_C0), dm_irq_prio(DM_IRQ_HW_C0));
	zassert_equal(DM_IRQ_PRIO_CONST(DM_IRQ_HW_C4), dm_irq_prio(DM_IRQ_HW_C4));
	zassert_equal(DM_IRQ_PRIO_CONST(DM_IRQ_EDGE), dm_irq_prio(DM_IRQ_EDGE));
}

ZTEST(prio, test_owner_defaults)
{
	struct dm_link_caps mac = {.tier = DM_TIER_MAC};
	struct dm_link_caps sw = {.tier = DM_TIER_SW};
	struct dm_link_caps metered = {.tier = DM_TIER_MAC, .metered = true};

	zassert_equal(dm_prio_owner_default(&mac), CONFIG_DM_SCHED_PRIO + 1);
	zassert_equal(dm_prio_owner_default(&sw), CONFIG_DM_SCHED_PRIO + 2);
	zassert_equal(dm_prio_owner_default(&metered), CONFIG_DM_SCHED_PRIO + 3);
}

ZTEST(prio, test_init_asserts)
{
	struct dm_prio_link_cfg ok[2] = {
		{.owner_prio = 5, .tier = DM_TIER_MAC},
		{.owner_prio = 6, .tier = DM_TIER_HW,
		 .hw_irqn = {20, 20, 20, 21, 21}, .hw_prio = {1, 1, 1, 3, 3}},
	};
	struct dm_prio_link_cfg c[2];

	zassert_equal(dm_prio_check(4, ok, 2), DM_PRIO_OK);
	zassert_equal(dm_prio_check(-1, ok, 2), DM_PRIO_SCHED_COOPERATIVE);

	memcpy(c, ok, sizeof(c));
	c[1].owner_prio = 4;
	zassert_equal(dm_prio_check(4, c, 2), DM_PRIO_SCHED_NOT_ABOVE_OWNER);

	memcpy(c, ok, sizeof(c));
	c[0].owner_prio = 3; /* a MAC-on-core link may sit above the scheduler */
	zassert_equal(dm_prio_check(4, c, 2), DM_PRIO_OK);

	memcpy(c, ok, sizeof(c));
	c[0].owner_prio = -1;
	zassert_equal(dm_prio_check(4, c, 2), DM_PRIO_OWNER_COOPERATIVE);

	memcpy(c, ok, sizeof(c));
	c[1].hw_irqn[DM_C4] = 20; /* C4 on the C0 line at another level */
	zassert_equal(dm_prio_check(4, c, 2), DM_PRIO_HW_IRQ_SHARED);

	memcpy(c, ok, sizeof(c));
	c[0].tier = DM_TIER_HW;
	c[0].hw_irqn[DM_C0] = 21; /* two links on one line */
	c[0].hw_prio[DM_C0] = 1;
	zassert_equal(dm_prio_check(4, c, 2), DM_PRIO_HW_IRQ_SHARED);

	memcpy(c, ok, sizeof(c));
	c[1].hw_prio[DM_C0] = 0;
	zassert_equal(dm_prio_check(4, c, 2), DM_PRIO_IRQ_LEVEL_ZERO);
}

ZTEST(prio, test_register_rejects_bad_priority)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	uint8_t link;

	cfg.caps.tier = DM_TIER_SW;
	cfg.caps.owner_prio = CONFIG_DM_SCHED_PRIO - 1;
	dm_fake_configure(0, &cfg);
	zassert_equal(dm_fake_register(0, &link), -EINVAL, "SW owner above the scheduler");

	cfg.caps.tier = DM_TIER_MAC;
	dm_fake_configure(0, &cfg);
	zassert_ok(dm_fake_register(0, &link), "MAC owner may sit above the scheduler");

	cfg.caps.owner_prio = 0;
	cfg.caps.owner_stack = CONFIG_DM_DEFAULT_OWNER_STACK + 1;
	dm_fake_configure(1, &cfg);
	zassert_equal(dm_fake_register(1, &link), -EINVAL, "stack larger than reserved");
}

ZTEST(prio, test_mac_prio_map)
{
	struct dm_link_caps c4 = {.tier = DM_TIER_MAC, .mac_prio_levels = 4};
	struct dm_link_caps c2 = {.tier = DM_TIER_MAC, .mac_prio_levels = 2};
	struct dm_link_caps c1 = {.tier = DM_TIER_MAC, .mac_prio_levels = 1};
	struct dm_link_caps sw = {.tier = DM_TIER_SW, .mac_prio_levels = 4};
	static const uint8_t want4[] = {0, 2, 1, 2, 3};
	static const uint8_t want2[] = {0, 0, 0, 1, 1};

	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		zassert_equal(dm_mac_prio_for(&c4, (dm_class_t)c), want4[c], "4 levels, C%d", c);
		zassert_equal(dm_mac_prio_for(&c2, (dm_class_t)c), want2[c], "2 levels, C%d", c);
		zassert_equal(dm_mac_prio_for(&c1, (dm_class_t)c), 0);
		zassert_equal(dm_mac_prio_for(&sw, (dm_class_t)c), 0, "SW tier ignores slots");
	}
}

ZTEST(prio, test_edca_backoff_bounds)
{
	uint32_t rng = 1;
	uint32_t lo = UINT32_MAX;
	uint32_t hi = 0;

	zassert_equal(dm_edca_backoff_us(0, &rng), 0);
	for (int i = 0; i < 2000; i++) {
		uint32_t b = dm_edca_backoff_us(8000, &rng);

		zassert_true(b <= 8000);
		lo = MIN(lo, b);
		hi = MAX(hi, b);
	}
	zassert_true(lo < 1000 && hi > 7000, "backoff covers the window (%u..%u)", lo, hi);
}
