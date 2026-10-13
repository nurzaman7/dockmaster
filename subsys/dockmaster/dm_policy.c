/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Policy artifact: validate, apply atomically, keep two persisted slots
 * (active and previous) and auto-revert after two quick reboots (spec
 * section 6).
 *
 * RAM holds three buffers so that apply never writes into the buffer that is
 * active or the one that was active just before; a reader that loaded the
 * active pointer keeps a consistent policy as long as it does not hold the
 * pointer across a blocking call.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#if defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include "dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

struct dm_boot_rec {
	uint16_t version; /* policy under trial */
	uint8_t trial;    /* applied, not yet confirmed by surviving the window */
	uint8_t boots;    /* reboots seen while on trial */
};

static struct dm_policy slots[3];
static atomic_ptr_t active;
static struct dm_policy *previous;
static K_MUTEX_DEFINE(apply_lock);
static struct k_timer confirm_timer;
static atomic_t confirm_due;
static const struct dm_policy_store *store;

/* ------------------------------------------------------------------ */
/* Storage backends                                                    */
/* ------------------------------------------------------------------ */

/* RAM store: what the tests use ("NVS emulated in RAM"). */
static struct {
	struct dm_policy a;
	struct dm_policy p;
	struct dm_boot_rec boot;
	bool has_a, has_p, has_boot;
} ram;

struct ram_slot {
	const char *key;
	void *data;
	bool *has;
	size_t len;
};

static const struct ram_slot *ram_find(const char *key)
{
	static const struct ram_slot table[] = {
		{"a", &ram.a, &ram.has_a, sizeof(ram.a)},
		{"p", &ram.p, &ram.has_p, sizeof(ram.p)},
		{"boot", &ram.boot, &ram.has_boot, sizeof(ram.boot)},
	};

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (strcmp(key, table[i].key) == 0) {
			return &table[i];
		}
	}
	return NULL;
}

static int ram_save(const char *key, const void *data, size_t len)
{
	const struct ram_slot *s = ram_find(key);

	if (s == NULL || len != s->len) {
		return -EINVAL;
	}
	memcpy(s->data, data, len);
	*s->has = true;
	return 0;
}

static int ram_load(const char *key, void *data, size_t len)
{
	const struct ram_slot *s = ram_find(key);

	if (s == NULL || len != s->len || !*s->has) {
		return -ENOENT;
	}
	memcpy(data, s->data, len);
	return (int)len;
}

static const struct dm_policy_store ram_store = {.save = ram_save, .load = ram_load};

#if defined(CONFIG_SETTINGS)
/* Settings store, key dm/policy/{a,p,boot}; NVS or any other backend. */
struct load_ctx {
	const char *key;
	void *data;
	size_t len;
	int rc;
};

static int settings_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		       void *param)
{
	struct load_ctx *ctx = param;
	const char *next;

	if (key == NULL || !settings_name_steq(key, ctx->key, &next) || next != NULL) {
		return 0;
	}
	if (len != ctx->len) {
		ctx->rc = -EINVAL;
		return 0;
	}
	ctx->rc = (int)read_cb(cb_arg, ctx->data, len);
	return 0;
}

static int settings_store_save(const char *key, const void *data, size_t len)
{
	char path[24];

	snprintk(path, sizeof(path), "dm/policy/%s", key);
	return settings_save_one(path, data, len);
}

static int settings_store_load(const char *key, void *data, size_t len)
{
	struct load_ctx ctx = {.key = key, .data = data, .len = len, .rc = -ENOENT};
	int rc = settings_load_subtree_direct("dm/policy", settings_cb, &ctx);

	return rc != 0 ? rc : ctx.rc;
}

static const struct dm_policy_store settings_store = {
	.save = settings_store_save,
	.load = settings_store_load,
};
#endif

static const struct dm_policy_store *pick_store(void)
{
#if defined(CONFIG_SETTINGS)
	if (settings_subsys_init() == 0) {
		return &settings_store;
	}
	LOG_WRN("settings unavailable, policy kept in RAM only");
#endif
	return &ram_store;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

uint32_t dm_policy_crc(const struct dm_policy *p)
{
	return crc32_ieee((const uint8_t *)p, offsetof(struct dm_policy, crc32));
}

void dm_policy_seal(struct dm_policy *p)
{
	p->crc32 = dm_policy_crc(p);
}

uint8_t dm_policy_mask(const struct dm_policy *p, dm_class_t c)
{
	return p->cls[c].link_mask;
}

static const char *const err_str[] = {
	[DM_POL_OK] = "ok",
	[DM_POL_BAD_MAGIC] = "bad magic",
	[DM_POL_BAD_CRC] = "bad CRC",
	[DM_POL_OLD_VERSION] = "version not newer",
	[DM_POL_UNKNOWN_LINK] = "link_mask names an unregistered link",
	[DM_POL_EMPTY_C0] = "C0 link_mask empty",
	[DM_POL_QUOTA_OVERFLOW] = "quotas exceed slab capacity",
	[DM_POL_RATE_TOO_HIGH] = "rate above 1000 ms/s",
	[DM_POL_BAD_THRESHOLDS] = "depth thresholds out of order",
	[DM_POL_C0_NO_RATE] = "C0 bucket has no rate on an allowed link",
};

enum dm_policy_err dm_policy_validate(const struct dm_policy *p, uint16_t cur_version,
				      uint8_t registered, uint32_t capacity)
{
	uint64_t quota = 0U;

	if (p->magic != DM_POLICY_MAGIC) {
		return DM_POL_BAD_MAGIC;
	}
	if (p->crc32 != dm_policy_crc(p)) {
		return DM_POL_BAD_CRC;
	}
	if (p->version <= cur_version) {
		return DM_POL_OLD_VERSION;
	}
	/* Hysteresis is 10 points below each threshold. */
	if (p->depth_tighten_pct[0] < 10U || p->depth_tighten_pct[0] >= p->depth_tighten_pct[1] ||
	    p->depth_tighten_pct[1] > 100U) {
		return DM_POL_BAD_THRESHOLDS;
	}
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_policy *cp = &p->cls[c];
		uint8_t mask = cp->link_mask;

		if (mask != DM_LINK_MASK_ALL && (mask & (uint8_t)~registered) != 0U) {
			return DM_POL_UNKNOWN_LINK;
		}
		if (c == DM_C0 && mask == 0U) {
			return DM_POL_EMPTY_C0;
		}
		quota += cp->quota_bytes;
		for (int l = 0; l < DM_MAX_LINKS; l++) {
			if (cp->bucket[l].rate_ms_per_s > 1000U) {
				return DM_POL_RATE_TOO_HIGH;
			}
			/* A zero C0 rate would block control forever after one overdraw. */
			if (c == DM_C0 && (mask & BIT(l)) != 0U && cp->bucket[l].rate_ms_per_s == 0U) {
				return DM_POL_C0_NO_RATE;
			}
		}
	}
	if (quota > capacity) {
		return DM_POL_QUOTA_OVERFLOW;
	}
	return DM_POL_OK;
}

/* ------------------------------------------------------------------ */
/* Slots                                                               */
/* ------------------------------------------------------------------ */

const struct dm_policy *dm_policy_current(void)
{
	struct dm_policy *p = atomic_ptr_get(&active);

	if (unlikely(p == NULL)) {
		/* Used before boot (a driver registering early): the default. */
		dm_policy_default(&slots[0]);
		(void)atomic_ptr_cas(&active, NULL, &slots[0]);
		p = atomic_ptr_get(&active);
	}
	return p;
}

static struct dm_policy *free_slot(void)
{
	const struct dm_policy *cur = atomic_ptr_get(&active);

	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		if (&slots[i] != cur && &slots[i] != previous) {
			return &slots[i];
		}
	}
	return &slots[0]; /* unreachable with three slots */
}

static bool image_ok(const struct dm_policy *p)
{
	return p->magic == DM_POLICY_MAGIC && p->crc32 == dm_policy_crc(p);
}

static void persist(bool trial)
{
	const struct dm_policy *cur = atomic_ptr_get(&active);
	struct dm_boot_rec rec = {.version = cur->version, .trial = trial ? 1U : 0U};

	(void)store->save("a", cur, sizeof(*cur));
	if (previous != NULL) {
		(void)store->save("p", previous, sizeof(*previous));
	}
	(void)store->save("boot", &rec, sizeof(rec));
}

static void start_confirm_timer(void)
{
	uint32_t win_s = dm_policy_current()->revert_window_s;

	if (win_s != 0U) {
		k_timer_start(&confirm_timer, K_SECONDS(win_s), K_NO_WAIT);
	}
}

static void confirm_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	atomic_set(&confirm_due, 1);
	dm_sched_kick();
}

/* Runs on the scheduler thread: the policy survived its trial window. */
void dm_policy_service(void)
{
	if (atomic_cas(&confirm_due, 1, 0)) {
		k_mutex_lock(&apply_lock, K_FOREVER);
		persist(false);
		k_mutex_unlock(&apply_lock);
		LOG_INF("policy v%u confirmed", dm_policy_current()->version);
	}
}

void dm_policy_boot(void)
{
	struct dm_boot_rec rec;

	store = pick_store();
	k_timer_init(&confirm_timer, confirm_fn, NULL);
	atomic_set(&confirm_due, 0);

	dm_policy_default(&slots[0]);
	atomic_ptr_set(&active, &slots[0]);
	previous = NULL;

	if (store->load("a", &slots[1], sizeof(slots[1])) == sizeof(slots[1]) &&
	    image_ok(&slots[1])) {
		atomic_ptr_set(&active, &slots[1]);
	}
	if (store->load("p", &slots[2], sizeof(slots[2])) == sizeof(slots[2]) &&
	    image_ok(&slots[2])) {
		previous = &slots[2];
	}
	if (store->load("boot", &rec, sizeof(rec)) != sizeof(rec) || rec.trial == 0U ||
	    rec.version != dm_policy_current()->version) {
		return;
	}
	rec.boots++;
	if (rec.boots >= 2U && previous != NULL) {
		struct dm_policy *bad = atomic_ptr_get(&active);

		LOG_WRN("policy v%u rebooted twice inside its window, reverting to v%u",
			bad->version, previous->version);
		atomic_ptr_set(&active, previous);
		previous = bad;
		persist(false);
		return;
	}
	(void)store->save("boot", &rec, sizeof(rec));
	start_confirm_timer();
}

int dm_policy_apply(const struct dm_policy *p)
{
	enum dm_policy_err e;

	if (p == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&apply_lock, K_FOREVER);
	struct dm_policy *cur = (struct dm_policy *)dm_policy_current();

	e = dm_policy_validate(p, cur->version, dm_registered_mask(), dm_slab_capacity_bytes());
	if (e != DM_POL_OK) {
		k_mutex_unlock(&apply_lock);
		LOG_WRN("policy v%u rejected: %s", p->version, err_str[e]);
		return -EINVAL;
	}
	struct dm_policy *next = free_slot();

	memcpy(next, p, sizeof(*next));
	previous = cur;
	atomic_ptr_set(&active, next);
	persist(true);
	start_confirm_timer();
	k_mutex_unlock(&apply_lock);

	LOG_INF("policy v%u applied (previous v%u)", next->version, cur->version);
	dm_sched_policy_changed();
	return 0;
}

int dm_policy_revert(void)
{
	k_mutex_lock(&apply_lock, K_FOREVER);
	if (previous == NULL) {
		k_mutex_unlock(&apply_lock);
		return -ENOENT;
	}
	struct dm_policy *was = atomic_ptr_get(&active);

	atomic_ptr_set(&active, previous);
	previous = was;
	k_timer_stop(&confirm_timer);
	persist(false);
	k_mutex_unlock(&apply_lock);

	LOG_WRN("policy reverted to v%u", dm_policy_current()->version);
	dm_sched_policy_changed();
	return 0;
}

#if defined(CONFIG_DM_TEST_HOOKS)
void dm_policy_test_reset(bool keep_store)
{
	k_timer_stop(&confirm_timer);
	if (!keep_store) {
		memset(&ram, 0, sizeof(ram));
#if defined(CONFIG_SETTINGS)
		(void)settings_delete("dm/policy/a");
		(void)settings_delete("dm/policy/p");
		(void)settings_delete("dm/policy/boot");
#endif
	}
	atomic_ptr_set(&active, NULL);
	previous = NULL;
}
#endif
