/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Policy artifact (spec section 6). Storage is the RAM store ("NVS emulated
 * in RAM"); a reboot is a full reset that keeps the store.
 */

#include "dm_test_common.h"

static void before(void *f)
{
	ARG_UNUSED(f);
	dmt_reset();
}

ZTEST_SUITE(policy, NULL, NULL, before, NULL, NULL);

static void reboot(void)
{
	dmt_reset_with(true);
}

ZTEST(policy, test_default_is_valid)
{
	struct dm_policy p;

	dm_policy_default(&p);
	zassert_equal(sizeof(p), 214, "packed layout changed");
	zassert_equal(dm_policy_validate(&p, 0, 0, dm_slab_capacity_bytes()), DM_POL_OK);
	zassert_equal(dm_policy_current()->version, 1);
	zassert_equal(dm_policy_current()->magic, DM_POLICY_MAGIC);
}

ZTEST(policy, test_reject_invalid)
{
	struct dm_fake_cfg cfg = dmt_link_cfg(false);
	struct dm_policy p;
	uint32_t crc_before;

	(void)dmt_add_link(0, &cfg);
	(void)dmt_add_link(1, &cfg);
	crc_before = dm_policy_current()->crc32;

	dmt_policy(&p, 2);
	p.magic = 0x12345678;
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_BAD_MAGIC);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 2);
	p.cls[DM_C2].max_age_ms = 1; /* edited after sealing */
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_BAD_CRC);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 1);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_OLD_VERSION);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 2);
	p.cls[DM_C3].link_mask = BIT(2); /* only links 0 and 1 exist */
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_UNKNOWN_LINK);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 2);
	p.cls[DM_C0].link_mask = 0;
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_EMPTY_C0);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 2);
	p.cls[DM_C4].quota_bytes += dm_slab_capacity_bytes();
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, dm_slab_capacity_bytes()),
		      DM_POL_QUOTA_OVERFLOW);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 2);
	p.cls[DM_C4].bucket[1].rate_ms_per_s = 1001;
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_RATE_TOO_HIGH);
	zassert_equal(dm_policy_apply(&p), -EINVAL);

	dmt_policy(&p, 2);
	p.depth_tighten_pct[0] = 90;
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_BAD_THRESHOLDS);

	dmt_policy(&p, 2);
	p.cls[DM_C0].link_mask = BIT(0);
	p.cls[DM_C0].bucket[0].rate_ms_per_s = 0;
	dm_policy_seal(&p);
	zassert_equal(dm_policy_validate(&p, 1, 0x3, 7680), DM_POL_C0_NO_RATE);

	zassert_equal(dm_policy_current()->version, 1, "nothing was applied");
	zassert_equal(dm_policy_current()->crc32, crc_before, "not even partially");

	dmt_policy(&p, 2);
	p.cls[DM_C3].link_mask = BIT(1);
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p), "a registered link bit is fine");
}

ZTEST(policy, test_revert_restores_previous)
{
	struct dm_policy p;

	zassert_equal(dm_policy_revert(), -ENOENT, "nothing to revert to yet");
	dmt_policy(&p, 2);
	p.cls[DM_C3].quota_bytes = 999;
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	zassert_equal(dm_policy_current()->cls[DM_C3].quota_bytes, 999);

	zassert_ok(dm_policy_revert());
	zassert_equal(dm_policy_current()->version, 1);
	zassert_not_equal(dm_policy_current()->cls[DM_C3].quota_bytes, 999);
	zassert_ok(dm_policy_revert(), "the two slots swap");
	zassert_equal(dm_policy_current()->version, 2);
}

ZTEST(policy, test_persisted_across_reboot)
{
	struct dm_policy p;

	dmt_policy(&p, 7);
	zassert_ok(dm_policy_apply(&p));
	reboot();
	zassert_equal(dm_policy_current()->version, 7, "active slot survives a reboot");
	zassert_ok(dm_policy_revert(), "previous slot survives too");
	zassert_equal(dm_policy_current()->version, 1);
}

ZTEST(policy, test_double_reboot_auto_revert)
{
	struct dm_policy p;

	dmt_policy(&p, 2);
	p.revert_window_s = 60;
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));

	reboot();
	zassert_equal(dm_policy_current()->version, 2, "one reboot inside the window is fine");
	reboot();
	zassert_equal(dm_policy_current()->version, 1, "two reboots inside the window revert");
	reboot();
	zassert_equal(dm_policy_current()->version, 1, "and the revert sticks");
}

ZTEST(policy, test_confirmed_after_window)
{
	struct dm_policy p;

	dmt_policy(&p, 3);
	p.revert_window_s = 1;
	dm_policy_seal(&p);
	zassert_ok(dm_policy_apply(&p));
	k_msleep(1100);
	dmt_round(); /* the scheduler thread confirms the trial */

	reboot();
	reboot();
	reboot();
	zassert_equal(dm_policy_current()->version, 3, "a confirmed policy is never reverted");
}

/* Producer that checks every policy it sees is whole while another thread applies. */
static volatile bool stop;
static volatile uint32_t torn;
static volatile uint32_t seen;

static void producer(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!stop) {
		const struct dm_policy *p = dm_policy_current();

		if (p->magic != DM_POLICY_MAGIC || p->crc32 != dm_policy_crc(p)) {
			torn++;
		}
		int rc = dmt_submit(DM_C3, seen, 32, 0, NULL);

		if (rc != 0 && rc != -ENOSPC && rc != -ENOMEM) {
			torn++;
		}
		seen++;
		k_usleep(50);
	}
}

K_THREAD_STACK_DEFINE(producer_stack, 2048);
static struct k_thread producer_thread;

ZTEST(policy, test_apply_atomic_under_concurrent_submit)
{
	struct dm_policy p;

	stop = false;
	torn = 0;
	seen = 0;
	k_thread_create(&producer_thread, producer_stack, K_THREAD_STACK_SIZEOF(producer_stack),
			producer, NULL, NULL, NULL, 9, 0, K_NO_WAIT);
	for (uint16_t v = 2; v < 60; v++) {
		dmt_policy(&p, v);
		p.cls[DM_C3].quota_bytes = (v % 2) ? 64 : 1500;
		dm_policy_seal(&p);
		zassert_ok(dm_policy_apply(&p));
		dmt_drain(4);
		k_usleep(130);
	}
	stop = true;
	k_thread_join(&producer_thread, K_FOREVER);
	zassert_true(seen > 50, "producer barely ran (%u)", seen);
	zassert_equal(torn, 0, "a producer saw a torn policy or an odd error");
	zassert_equal(dm_policy_current()->version, 59);
}
