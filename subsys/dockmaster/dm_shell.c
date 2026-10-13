/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell commands (spec section 13).
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include "dm_internal.h"

static const char *const pressure_names[] = {"NORMAL", "TIGHT", "SHED", "CRITICAL"};
static const char *const state_names[] = {"DOWN", "DEGRADED", "UP"};

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct dm_stats s;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	dm_stats_get(&s);
	shell_print(sh, "policy v%u  pressure %s  slab %u%%  c0_lost %u  rounds %u  max %u us",
		    s.policy_version, pressure_names[s.pressure_state], s.slab_pct, s.c0_lost,
		    s.sched_rounds, s.sched_max_us);
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_stats *k = &s.cls[c];

		shell_print(sh,
			    "C%d sub %u refused %u sent %u failed %u drop q/a/s %u/%u/%u coal %u "
			    "queued %u B oldest %u ms air %u ms",
			    c, k->submitted, k->refused, k->sent, k->failed, k->dropped_quota,
			    k->dropped_age, k->dropped_shed, k->coalesced, k->bytes_queued,
			    k->oldest_unsent_ms, k->airtime_ms_used);
	}
	for (uint8_t l = 0; l < dm_link_count(); l++) {
		const struct dm_link_stats *k = &s.link[l];

		shell_print(sh,
			    "L%u %s tier %u ok %u fail %u faults %u probes %u/%u switches %u "
			    "rtt %u us tput %u bps loss %u.%u%% score %u busy %u ms",
			    l, state_names[k->state], k->cur_tier, k->tx_ok, k->tx_fail, k->faults,
			    k->probes_ok, k->probes_fail, k->switches_to, k->ewma_rtt_us,
			    k->ewma_tput_bps, k->ewma_loss_pct_x10 / 10U, k->ewma_loss_pct_x10 % 10U,
			    k->score, k->busy_wait_ms);
	}
	return 0;
}

static int cmd_policy_show(const struct shell *sh, size_t argc, char **argv)
{
	const struct dm_policy *p = dm_policy_current();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "policy v%u  revert window %u s  thresholds %u%%/%u%%  crc 0x%08x",
		    p->version, p->revert_window_s, p->depth_tighten_pct[0],
		    p->depth_tighten_pct[1], p->crc32);
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		const struct dm_class_policy *k = &p->cls[c];

		shell_print(sh, "C%d quota %u B age %u ms mask 0x%02x flags 0x%02x bucket0 %u/%u",
			    c, k->quota_bytes, k->max_age_ms, k->link_mask, k->flags,
			    k->bucket[0].rate_ms_per_s, k->bucket[0].burst_ms);
	}
	return 0;
}

static int cmd_policy_revert(const struct shell *sh, size_t argc, char **argv)
{
	int rc = dm_policy_revert();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc != 0) {
		shell_error(sh, "revert failed: %d", rc);
		return rc;
	}
	shell_print(sh, "now on policy v%u", dm_policy_current()->version);
	return 0;
}

static int cmd_link(const struct shell *sh, size_t argc, char **argv)
{
	long n = strtol(argv[1], NULL, 0);

	ARG_UNUSED(argc);
	if (n < 0 || n >= dm_link_count()) {
		shell_error(sh, "no link %ld", n);
		return -EINVAL;
	}
	if (strcmp(argv[2], "probe") == 0) {
		atomic_set(&dm_links[n].probe_due, 1);
		dm_sched_kick();
	} else if (strcmp(argv[2], "down") == 0) {
		dm_link_state((uint8_t)n, DM_LINK_DOWN);
	} else if (strcmp(argv[2], "up") == 0) {
		dm_link_state((uint8_t)n, DM_LINK_UP);
	} else {
		shell_error(sh, "expected probe, down or up");
		return -EINVAL;
	}
	return 0;
}

static int cmd_submit(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t buf[CONFIG_DM_MAX_MSG];
	long cls = strtol(argv[1], NULL, 0);
	long len = strtol(argv[2], NULL, 0);
	int rc;

	ARG_UNUSED(argc);
	if (cls < 0 || cls >= DM_CLASS_COUNT || len <= 0 || len > CONFIG_DM_MAX_MSG) {
		shell_error(sh, "usage: dm submit <0..4> <1..%d>", CONFIG_DM_MAX_MSG);
		return -EINVAL;
	}
	for (long i = 0; i < len; i++) {
		buf[i] = (uint8_t)(0xA5 ^ i);
	}
	rc = dm_submit((dm_class_t)cls, buf, (size_t)len, 0U, NULL);
	shell_print(sh, "dm_submit(C%ld, %ld) = %d", cls, len, rc);
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_dm_policy,
	SHELL_CMD(show, NULL, "Print the active policy", cmd_policy_show),
	SHELL_CMD(revert, NULL, "Go back to the previous policy", cmd_policy_revert),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_dm,
	SHELL_CMD(stats, NULL, "Print every counter", cmd_stats),
	SHELL_CMD(policy, &sub_dm_policy, "Policy commands", NULL),
	SHELL_CMD_ARG(link, NULL, "<n> probe|down|up (down/up for testing)", cmd_link, 3, 0),
	SHELL_CMD_ARG(submit, NULL, "<cls> <len>: submit a pattern", cmd_submit, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(dm, &sub_dm, "Dockmaster", NULL);
