/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Init, link registration and the init-time priority asserts (spec
 * sections 9 and 11).
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_DM_GPIO_PROBES)
#include <zephyr/drivers/gpio.h>
#endif

#include "dm_internal.h"

LOG_MODULE_REGISTER(dm, CONFIG_DM_LOG_LEVEL);

struct dm_link dm_links[CONFIG_DM_LINK_SLOTS];
uint8_t dm_n_links;

K_THREAD_STACK_ARRAY_DEFINE(dm_owner_stacks, CONFIG_DM_LINK_SLOTS, CONFIG_DM_DEFAULT_OWNER_STACK);

static bool core_ready;
static bool running;
static bool sched_thread_wanted = true;

#if defined(CONFIG_DM_TEST_HOOKS)
static bool clock_fake;
static uint32_t clock_now;
#endif

uint32_t dm_now(void)
{
#if defined(CONFIG_DM_TEST_HOOKS)
	if (clock_fake) {
		return clock_now;
	}
#endif
	return (uint32_t)k_uptime_ticks();
}

/* Ticks, not k_uptime_get_32(): no 64-bit divide on the shed path. */
bool dm_log_ratelimit(uint32_t *last_tick)
{
	uint32_t now = (uint32_t)k_uptime_ticks();

	if (*last_tick != 0U && now - *last_tick < CONFIG_SYS_CLOCK_TICKS_PER_SEC) {
		return false;
	}
	*last_tick = now == 0U ? 1U : now;
	return true;
}

#if defined(CONFIG_DM_GPIO_PROBES)
#define PROBES_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(dockmaster_gpio_probes)
static const struct gpio_dt_spec probe_a = GPIO_DT_SPEC_GET_BY_IDX(PROBES_NODE, gpios, 0);
static const struct gpio_dt_spec probe_b = GPIO_DT_SPEC_GET_BY_IDX(PROBES_NODE, gpios, 1);

void dm_probe_a_toggle(void)
{
	(void)gpio_pin_toggle_dt(&probe_a);
}

void dm_probe_b_toggle(void)
{
	(void)gpio_pin_toggle_dt(&probe_b);
}

static void probes_init(void)
{
	(void)gpio_pin_configure_dt(&probe_a, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&probe_b, GPIO_OUTPUT_INACTIVE);
}
#else
static void probes_init(void)
{
}
#endif

/*
 * Module state that must exist before the first registration or submit.
 * Drivers register at POST_KERNEL, before storage may be usable, so the
 * stored policy is loaded later by dm_init(); until then the built-in
 * default applies.
 */
static void core_init_once(void)
{
	if (core_ready) {
		return;
	}
	dm_queue_init();
	dm_stats_clear();
	dm_pressure_reset();
	dm_lm_init();
	dm_sched_init();
	probes_init();
	core_ready = true;
}

static void probe_timer_fn(struct k_timer *t)
{
	struct dm_link *l = CONTAINER_OF(t, struct dm_link, probe_timer);

	atomic_set(&l->probe_due, 1);
	dm_sched_kick();
}

uint8_t dm_link_count(void)
{
	return dm_n_links;
}

int dm_link_register(const struct device *dev, const struct dm_link_ops *ops,
		     const struct dm_link_caps *caps, uint8_t *out_index)
{
	struct dm_link *l;
	enum dm_prio_err pe;
	int prio;

	if (ops == NULL || ops->tx == NULL || ops->cost_of_us == NULL || caps == NULL ||
	    out_index == NULL || caps->tier > DM_TIER_HW || caps->mtu == 0U) {
		return -EINVAL;
	}
	if (caps->owner_stack > CONFIG_DM_DEFAULT_OWNER_STACK) {
		LOG_ERR("owner stack %u > CONFIG_DM_DEFAULT_OWNER_STACK", (unsigned int)caps->owner_stack);
		return -EINVAL;
	}
	prio = caps->owner_prio != 0U ? caps->owner_prio : dm_prio_owner_default(caps);
	pe = dm_prio_check_owner(CONFIG_DM_SCHED_PRIO, prio, caps->tier);
	if (pe != DM_PRIO_OK) {
		LOG_ERR("link rejected: %s", dm_prio_err_str(pe));
		return -EINVAL;
	}
	core_init_once();
	if (dm_n_links >= CONFIG_DM_LINK_SLOTS) {
		return -ENOSPC;
	}

	l = &dm_links[dm_n_links];
	memset(l, 0, sizeof(*l));
	l->dev = dev;
	l->ops = ops;
	l->caps = *caps;
	l->index = dm_n_links;
	l->prio = prio;
	k_msgq_init(&l->dispatch_q, (char *)l->dispatch_buf, sizeof(struct dm_dispatch),
		    ARRAY_SIZE(l->dispatch_buf));
	k_sem_init(&l->done_sem, 0, 1);
	k_timer_init(&l->probe_timer, probe_timer_fn, NULL);
	atomic_set(&l->reported_state, DM_LINK_UP);

	/* Publish only once the slot is complete. */
	compiler_barrier();
	dm_n_links++;
	dm_sched_link_added(l->index, dm_now());
	dm_lm_link_added(l->index, dm_now());
	*out_index = l->index;
	LOG_INF("link %u registered: tier %u, mtu %u, owner prio %d%s", l->index, caps->tier,
		caps->mtu, prio, caps->metered ? ", metered" : "");

	if (running) {
		dm_owner_start(l, dm_owner_stacks[l->index],
			       K_THREAD_STACK_SIZEOF(dm_owner_stacks[l->index]));
	}
	dm_sched_kick();
	return 0;
}

static enum dm_prio_err check_all(void)
{
	struct dm_prio_link_cfg cfg[CONFIG_DM_LINK_SLOTS];

	for (uint8_t i = 0; i < dm_n_links; i++) {
		cfg[i].owner_prio = dm_links[i].prio;
		cfg[i].tier = dm_links[i].caps.tier;
		memcpy(cfg[i].hw_irqn, dm_links[i].hw_irqn, sizeof(cfg[i].hw_irqn));
		memcpy(cfg[i].hw_prio, dm_links[i].hw_prio, sizeof(cfg[i].hw_prio));
	}
	return dm_prio_check(CONFIG_DM_SCHED_PRIO, cfg, dm_n_links);
}

int dm_init(void)
{
	enum dm_prio_err pe;

	if (running) {
		return 0;
	}
	core_init_once();
	dm_policy_boot();
	dm_sched_policy_changed();
	pe = check_all();
	if (pe != DM_PRIO_OK) {
		LOG_ERR("init assert failed: %s", dm_prio_err_str(pe));
		return -EINVAL;
	}
	dm_sched_start(sched_thread_wanted);
	for (uint8_t i = 0; i < dm_n_links; i++) {
		dm_owner_start(&dm_links[i], dm_owner_stacks[i],
			       K_THREAD_STACK_SIZEOF(dm_owner_stacks[i]));
	}
	running = true;
	LOG_INF("dockmaster up: %u link(s), policy v%u", dm_n_links,
		dm_policy_current()->version);
	return 0;
}

static int dm_sys_init(void)
{
	return dm_init();
}

SYS_INIT(dm_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_DM_TEST_HOOKS)
void dm_test_clock_set(uint32_t now)
{
	clock_fake = true;
	clock_now = now;
}

void dm_test_clock_advance_ms(uint32_t ms)
{
	clock_now += dm_ms_to_ticks(ms);
}

void dm_test_reset(const struct dm_test_cfg *cfg)
{
	static const struct dm_test_cfg defaults;

	if (cfg == NULL) {
		cfg = &defaults;
	}
	dm_sched_stop();
	for (uint8_t i = 0; i < dm_n_links; i++) {
		k_timer_stop(&dm_links[i].probe_timer);
		if (dm_links[i].started) {
			k_thread_abort(&dm_links[i].thread);
		}
	}
	memset(dm_links, 0, sizeof(dm_links));
	dm_n_links = 0U;
	dm_policy_test_reset(cfg->keep_store);
	clock_fake = cfg->fake_clock;
	clock_now = cfg->clock;
	core_ready = false;
	running = false;
	sched_thread_wanted = !cfg->manual_sched;
	(void)dm_init();
}
#endif
