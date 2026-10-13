/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Link manager (spec section 10). Answers one question per class: which
 * eligible link is preferred right now. Runs inside the scheduler thread.
 * Inputs are only what Dockmaster measures (delivered throughput, probe RTT,
 * loss); a modem's own RSSI or RSRP is never used.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dm_internal.h"

LOG_MODULE_DECLARE(dm, CONFIG_DM_LOG_LEVEL);

#define TPUT_FULL_BPS 10000000U /* 10 Mbit/s scores 1000 */
#define RTT_FULL_US   10000000U /* 10 s scores 1000 */
#define TPUT_MAX_BPS  100000000U
#define RTT_MAX_US    60000000U

struct lm_link {
	uint32_t tput_acc; /* EWMA accumulators, value << 3 */
	uint32_t rtt_acc;
	uint32_t loss_acc;
	uint32_t health_tick;
	uint32_t recover_tick;
	bool verified;
	uint8_t reported;
	uint8_t health;
	uint8_t eff;
	uint8_t probe_fails;
	uint8_t faults;
	bool dirty;     /* inputs changed since the score was computed */
	uint16_t score; /* without flap penalty: what the current link is judged by */
};

/*
 * The flap penalty is kept per class and per link and applied only to
 * candidates: it starts at 1000 when the class switches to a link and halves
 * every half-life, so a class that keeps bouncing between links has to clear
 * a growing bar. Keeping it per class stops one class's switch from holding
 * every other class back (see doc/deviations.md).
 */
struct lm_class {
	uint8_t cur;
	uint8_t alt;
	uint8_t mbb; /* make-before-break target waiting for its probe */
	uint8_t elig;
	uint8_t flap_on;
	uint32_t held_since;
	uint32_t flap_tick[DM_MAX_LINKS];
};

static struct lm_link lk[DM_MAX_LINKS];
static struct lm_class lc[DM_CLASS_COUNT];
static bool dup_forced;

static const uint32_t dwell_ms[DM_CLASS_COUNT] = {0U, 2000U, 5000U, 30000U, 300000U};

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

/* Shift based EWMA with gain 1/8, kept in Q3 so it converges exactly. */
void dm_ewma_q3(uint32_t *acc, uint32_t x)
{
	*acc = *acc + x - (*acc >> 3);
}

static uint32_t log2_q8(uint32_t x)
{
	if (x == 0U) {
		return 0U;
	}
	uint32_t n = 31U - (uint32_t)__builtin_clz(x);
	uint32_t frac = n >= 8U ? (x >> (n - 8U)) & 0xFFU : (x << (8U - n)) & 0xFFU;

	return (n << 8) | frac;
}

/*
 * Radios differ by orders of magnitude (a few kbit/s to Mbit/s), so rates
 * and delays are normalised on a log scale to 0..1000.
 */
uint16_t dm_lm_norm_log(uint32_t x, uint32_t full_scale)
{
	uint32_t n = log2_q8(x) * 1000U / log2_q8(full_scale);

	return (uint16_t)MIN(n, 1000U);
}

uint16_t dm_lm_flap_decay(uint32_t elapsed_ms, uint32_t halflife_ms)
{
	uint32_t halvings = elapsed_ms / halflife_ms;

	return halvings >= 10U ? 0U : (uint16_t)(1000U >> halvings);
}

uint32_t dm_lm_dwell_ms(dm_class_t c)
{
	return dwell_ms[c];
}

/* Section 10 switch rule: beat the current score by the margin, after dwell. */
bool dm_lm_switch_ok(uint16_t best, uint16_t cur, uint32_t held_ms, uint32_t dwell)
{
	uint32_t bar = cur + (uint32_t)cur * CONFIG_DM_SWITCH_MARGIN_PCT / 100U;

	return best > bar && held_ms >= dwell;
}

/* Integer score in 0..1000 (section 10). */
uint16_t dm_lm_score_calc(const struct dm_lm_inputs *in)
{
	if (in->state == DM_LINK_DOWN) {
		return 0U;
	}
	/* Every term is at most 1000 x 1000, so 32 bits are plenty. */
	int32_t tput = dm_lm_norm_log(in->tput_bps, TPUT_FULL_BPS);
	int32_t rtt = dm_lm_norm_log(in->rtt_us, RTT_FULL_US);
	int32_t loss = MIN(in->loss_x10, 1000U);
	int32_t state = in->state == DM_LINK_UP ? 1000 : 400;
	int32_t raw = CONFIG_DM_W_TPUT * tput - CONFIG_DM_W_RTT * rtt - CONFIG_DM_W_LOSS * loss -
		      CONFIG_DM_W_FLAP * (int32_t)in->flap -
		      CONFIG_DM_W_COST * (in->metered ? 1000 : 0) + CONFIG_DM_W_STATE * state;
	int32_t neg = (CONFIG_DM_W_RTT + CONFIG_DM_W_LOSS + CONFIG_DM_W_FLAP + CONFIG_DM_W_COST) *
		      1000;
	int32_t s = (raw + neg) / 1000;

	return (uint16_t)CLAMP(s, 0, 1000);
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static uint32_t elapsed_ms(uint32_t now, uint32_t since)
{
	return dm_ticks_to_ms(now - since);
}

static uint16_t flap_now(struct lm_class *cl, uint8_t l, uint32_t now)
{
	if ((cl->flap_on & BIT(l)) == 0U) {
		return 0U;
	}
	uint16_t f = dm_lm_flap_decay(elapsed_ms(now, cl->flap_tick[l]),
				      CONFIG_DM_FLAP_HALFLIFE_S * 1000U);

	if (f == 0U) {
		cl->flap_on &= (uint8_t)~BIT(l);
	}
	return f;
}

static void set_health(uint8_t l, uint8_t h, uint32_t now)
{
	struct lm_link *k = &lk[l];

	if (k->health == h) {
		return;
	}
	LOG_INF("link %u health %u -> %u", l, k->health, h);
	k->health = h;
	k->health_tick = now;
	k->recover_tick = now;
	if (h == DM_LINK_DOWN) {
		k->verified = dm_links[l].ops->probe == NULL;
		dm_lm_request_probe(l);
	}
}

/* One more failure: UP degrades at the threshold, DEGRADED goes DOWN. */
static void on_failure(uint8_t l, uint8_t count, uint8_t threshold, uint32_t now)
{
	struct lm_link *k = &lk[l];

	if (k->health == DM_LINK_UP && count >= threshold) {
		set_health(l, DM_LINK_DEGRADED, now);
	} else if (k->health == DM_LINK_DEGRADED && count > threshold) {
		set_health(l, DM_LINK_DOWN, now);
	}
}

static void update_eff(uint8_t l)
{
	struct lm_link *k = &lk[l];
	uint8_t eff = DM_LINK_DOWN;

	if (k->reported != DM_LINK_DOWN && k->health != DM_LINK_DOWN && k->verified) {
		eff = MIN(k->reported, k->health);
	}
	if (eff != k->eff) {
		k->eff = eff;
		k->dirty = true;
	}
}

static void rescore(uint8_t l)
{
	struct lm_link *k = &lk[l];

	if (k->dirty) {
		struct dm_lm_inputs in = {
			.tput_bps = k->tput_acc >> 3,
			.rtt_us = k->rtt_acc >> 3,
			.loss_x10 = (uint16_t)(k->loss_acc >> 3),
			.metered = dm_links[l].caps.metered,
			.state = k->eff,
		};

		k->score = dm_lm_score_calc(&in);
		k->dirty = false;
	}
}

/*
 * Score of @p l as a switch candidate for class @p c. The flap term is
 * linear in the score formula, so it is subtracted from the cached score
 * instead of recomputing the whole formula per class.
 */
static uint16_t cand_score(dm_class_t c, uint8_t l, uint32_t now)
{
	struct lm_class *cl = &lc[c];
	uint32_t pen = l == cl->cur ? 0U : CONFIG_DM_W_FLAP * flap_now(cl, l, now) / 1000U;

	return lk[l].score > pen ? (uint16_t)(lk[l].score - pen) : 0U;
}

void dm_lm_init(void)
{
	memset(lk, 0, sizeof(lk));
	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		lc[c] = (struct lm_class){.cur = DM_NO_LINK, .alt = DM_NO_LINK, .mbb = DM_NO_LINK};
	}
	dup_forced = false;
}

void dm_lm_link_added(uint8_t l, uint32_t now)
{
	struct dm_link *dl = &dm_links[l];
	struct lm_link *k = &lk[l];
	uint16_t mtu = MAX(dl->caps.mtu, 1U);
	uint32_t cost = MAX(dl->ops->cost_of_us(dl->dev, mtu), 1U);

	/* Priors from the driver's own airtime model until measurements arrive. */
	*k = (struct lm_link){
		.tput_acc = (uint32_t)MIN((uint64_t)mtu * 8U * 1000000U / cost, TPUT_MAX_BPS) << 3,
		.rtt_acc = MIN(2U * cost, RTT_MAX_US) << 3,
		.verified = true,
		.reported = DM_LINK_UP,
		.health = DM_LINK_UP,
		.health_tick = now,
		.recover_tick = now,
		.eff = DM_LINK_UP,
		.dirty = true,
	};
	atomic_set(&dl->reported_state, DM_LINK_UP);
	update_eff(l);
	rescore(l);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

void dm_lm_request_probe(uint8_t l)
{
	if (dm_links[l].ops->probe != NULL) {
		dm_links[l].probe_req = true;
	}
}

void dm_lm_on_tx(uint8_t l, uint16_t len, int status, uint32_t airtime_us, bool fault,
		 uint32_t now)
{
	struct lm_link *k = &lk[l];

	if (status == 0 && airtime_us != 0U) {
		/* bytes x 8e6 overflows 32 bits above 536 bytes: scale first. */
		uint32_t bps = len <= 536U ? (uint32_t)len * 8000000U / airtime_us
					   : (uint32_t)len * 8000U / MAX(airtime_us / 1000U, 1U);

		dm_ewma_q3(&k->tput_acc, MIN(bps, TPUT_MAX_BPS));
	}
	dm_ewma_q3(&k->loss_acc, status == 0 ? 0U : 1000U);
	k->dirty = true;
	if (status == 0) {
		k->faults = 0U;
		if (k->health == DM_LINK_DEGRADED && k->probe_fails == 0U &&
		    elapsed_ms(now, k->health_tick) >= DM_RECOVERY_MS) {
			set_health(l, DM_LINK_UP, now);
		}
	} else if (fault) {
		k->faults++;
		on_failure(l, k->faults, CONFIG_DM_FAULT_N, now);
	}
}

static void commit(dm_class_t c, uint8_t to, uint32_t now)
{
	struct lm_class *cl = &lc[c];

	LOG_INF("C%d: link %u -> %u", c, cl->cur, to);
	cl->cur = to;
	cl->held_since = now;
	cl->mbb = DM_NO_LINK;
	cl->flap_on |= (uint8_t)BIT(to);
	cl->flap_tick[to] = now;
	dm_ctr.link[to].switches_to++;
	DM_TRACE("dm_switch", c, to, 0U);
}

static void on_probe(uint8_t l, uint32_t rtt_us, uint8_t loss, uint32_t now)
{
	struct lm_link *k = &lk[l];
	bool ok = loss < 100U;

	k->dirty = true;

	if (ok) {
		dm_ctr.link[l].probes_ok++;
		k->probe_fails = 0U;
		dm_ewma_q3(&k->rtt_acc, MIN(rtt_us, RTT_MAX_US));
		dm_ewma_q3(&k->loss_acc, (uint32_t)loss * 10U);
		k->verified = true;
		if (k->health == DM_LINK_DOWN) {
			set_health(l, DM_LINK_UP, now);
		}
	} else {
		dm_ctr.link[l].probes_fail++;
		k->probe_fails++;
		dm_ewma_q3(&k->loss_acc, 1000U);
		on_failure(l, k->probe_fails, CONFIG_DM_PROBE_FAIL_N, now);
	}
	update_eff(l);
	rescore(l);

	for (int c = 0; c < DM_CLASS_COUNT; c++) {
		if (lc[c].mbb != l) {
			continue;
		}
		if (ok && k->eff != DM_LINK_DOWN) {
			commit((dm_class_t)c, l, now);
		} else {
			LOG_INF("C%d: switch to link %u cancelled by probe", c, l);
			lc[c].mbb = DM_NO_LINK;
		}
	}
}

static void poll_link(uint8_t l, uint32_t now)
{
	struct dm_link *dl = &dm_links[l];
	struct lm_link *k = &lk[l];
	uint8_t rep = (uint8_t)atomic_get(&dl->reported_state);
	uint32_t seq = (uint32_t)atomic_get(&dl->probe_seq);

	if (rep != k->reported) {
		if (k->reported == DM_LINK_DOWN && rep != DM_LINK_DOWN) {
			/* Back from DOWN: needs one passed probe first. */
			k->verified = dl->ops->probe == NULL;
			dm_lm_request_probe(l);
			k->recover_tick = now;
		}
		k->reported = rep;
	}
	if (atomic_cas(&dl->probe_due, 1, 0)) {
		dm_lm_request_probe(l);
	}
	if (seq != dl->probe_seq_seen) {
		dl->probe_seq_seen = seq;
		dl->probe_inflight = false;
		on_probe(l, dl->probe_rtt_us, dl->probe_loss, now);
	} else if (dl->probe_inflight &&
		   elapsed_ms(now, dl->probe_sent_tick) >= DM_PROBE_TIMEOUT_MS) {
		dl->probe_inflight = false;
		on_probe(l, 0U, 100U, now);
	}
	/* Keep trying to recover a link that is down or unverified. */
	if ((k->health == DM_LINK_DOWN || !k->verified) &&
	    elapsed_ms(now, k->recover_tick) >= DM_RECOVERY_MS) {
		k->recover_tick = now;
		if (dl->ops->probe != NULL) {
			dm_lm_request_probe(l);
		} else if (k->health == DM_LINK_DOWN) {
			set_health(l, DM_LINK_DEGRADED, now); /* trial without a probe */
		}
	}
	update_eff(l);
	rescore(l);
}

static uint8_t best_in(dm_class_t c, uint8_t mask, uint8_t prefer, uint32_t now,
		       uint16_t *score)
{
	uint8_t best = DM_NO_LINK;
	uint16_t best_s = 0U;

	for (uint8_t l = 0; l < dm_n_links; l++) {
		if ((mask & BIT(l)) == 0U) {
			continue;
		}
		uint16_t sc = cand_score(c, l, now);

		if (best == DM_NO_LINK || sc > best_s || (sc == best_s && l == prefer)) {
			best = l;
			best_s = sc;
		}
	}
	if (score != NULL) {
		*score = best_s;
	}
	return best;
}

static uint8_t usable_mask(void)
{
	uint8_t m = 0U;

	for (uint8_t l = 0; l < dm_n_links; l++) {
		if (lk[l].eff != DM_LINK_DOWN) {
			m |= BIT(l);
		}
	}
	return m;
}

static void reassign(uint32_t now)
{
	const struct dm_policy *p = dm_policy_current();
	uint8_t usable = usable_mask();
	uint8_t metered = 0U;
	bool degraded = false;

	for (uint8_t l = 0; l < dm_n_links; l++) {
		if (dm_links[l].caps.metered) {
			metered |= BIT(l);
		}
		degraded = degraded || lk[l].eff == DM_LINK_DEGRADED;
	}
	dup_forced = degraded && POPCOUNT(usable) >= 2;

	for (int ci = 0; ci < DM_CLASS_COUNT; ci++) {
		dm_class_t c = (dm_class_t)ci;
		struct lm_class *cl = &lc[c];
		uint8_t e = dm_policy_mask(p, c) & usable;

		if ((p->cls[c].flags & DM_P_EXCLUSIVE_NO_METERED) != 0U) {
			e &= (uint8_t)~metered;
		}
		cl->elig = e;
		if (e == 0U) {
			cl->cur = DM_NO_LINK;
			cl->alt = DM_NO_LINK;
			cl->mbb = DM_NO_LINK;
			continue;
		}
		uint16_t best_s;
		uint8_t best = best_in(c, e, cl->cur, now, &best_s);

		if (cl->cur == DM_NO_LINK || (e & BIT(cl->cur)) == 0U) {
			if (cl->cur != DM_NO_LINK) {
				commit(c, best, now); /* failover: nothing to make before */
			} else {
				cl->cur = best;
				cl->held_since = now;
				cl->mbb = DM_NO_LINK;
			}
		} else if (best != cl->cur) {
			if (dm_lm_switch_ok(best_s, lk[cl->cur].score,
					    elapsed_ms(now, cl->held_since), dwell_ms[c])) {
				if (dm_links[best].ops->probe == NULL) {
					commit(c, best, now);
				} else if (cl->mbb != best) {
					cl->mbb = best;
					dm_lm_request_probe(best);
				}
			}
		} else {
			cl->mbb = DM_NO_LINK;
		}
		cl->alt = best_in(c, e & (uint8_t)~BIT(cl->cur), DM_NO_LINK, now, NULL);
	}
}

void dm_lm_service(uint32_t now)
{
	for (uint8_t l = 0; l < dm_n_links; l++) {
		poll_link(l, now);
	}
	reassign(now);
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

bool dm_lm_dup_forced(void)
{
	return dup_forced;
}

bool dm_lm_dup_class(dm_class_t c)
{
	return (dm_policy_current()->cls[c].flags & DM_P_DUP) != 0U ||
	       (c == DM_C0 && dup_forced);
}

bool dm_lm_serves(dm_class_t c, uint8_t l)
{
	return lc[c].cur == l || (dm_lm_dup_class(c) && lc[c].alt == l);
}

uint8_t dm_lm_eligible(dm_class_t c)
{
	return lc[c].elig;
}

uint8_t dm_lm_current(dm_class_t c)
{
	return lc[c].cur;
}

uint8_t dm_lm_alt(dm_class_t c)
{
	return lc[c].alt;
}

uint16_t dm_lm_score(uint8_t l)
{
	return lk[l].score;
}

enum dm_link_state dm_lm_state(uint8_t l)
{
	return (enum dm_link_state)lk[l].eff;
}

void dm_lm_link_stats(uint8_t l, struct dm_link_stats *out)
{
	const struct lm_link *k = &lk[l];

	out->ewma_rtt_us = k->rtt_acc >> 3;
	out->ewma_tput_bps = k->tput_acc >> 3;
	out->ewma_loss_pct_x10 = (uint16_t)(k->loss_acc >> 3);
	out->score = k->score;
	out->state = k->eff;
	out->cur_tier = dm_links[l].caps.tier;
}
