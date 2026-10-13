/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake link (spec section 9). Every unit test and the native_sim sample run
 * on it. Nothing here touches hardware, so it also runs on real boards.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <dockmaster/dm_link.h>
#include <dockmaster/dm_link_fake.h>

struct dm_fake {
	uint8_t idx;
	uint8_t link;
	bool registered;
	struct dm_fake_cfg cfg;
	struct dm_fake_counters ctr;
	uint32_t rng;
	uint32_t next_handle;
	/* In-flight transmission. */
	bool outstanding;
	uint32_t handle;
	const uint8_t *buf;
	size_t len;
	struct k_timer tx_timer;
	struct k_timer probe_timer;
};

static struct dm_fake fakes[CONFIG_DM_LINK_FAKE_COUNT];
static dm_fake_tx_cb_t tx_cb;

#define FAKE_DEFINE(i, _)                                                                      \
	DEVICE_DEFINE(dm_fake_##i, "dm_fake" #i, NULL, NULL, &fakes[i], NULL, POST_KERNEL, 99,  \
		      NULL)
LISTIFY(CONFIG_DM_LINK_FAKE_COUNT, FAKE_DEFINE, (;));

#define FAKE_GET(i, _) DEVICE_GET(dm_fake_##i)
static const struct device *const fake_devs[] = {
	LISTIFY(CONFIG_DM_LINK_FAKE_COUNT, FAKE_GET, (,))
};

static struct dm_fake *fake_of(const struct device *dev)
{
	return dev->data;
}

static uint32_t rnd(struct dm_fake *f)
{
	uint32_t x = f->rng != 0U ? f->rng : 0xA341316CU;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	f->rng = x;
	return x;
}

static uint32_t fake_cost_of_us(const struct device *dev, size_t len)
{
	const struct dm_fake *f = fake_of(dev);
	uint32_t bps = f->cfg.bitrate_bps != 0U ? f->cfg.bitrate_bps : 250000U;

	/* 32-bit only, like a real driver should be: len x 8000 / kbit/s. */
	return f->cfg.overhead_us + (uint32_t)len * 8000U / MAX(bps / 1000U, 1U);
}

static void complete(struct dm_fake *f)
{
	unsigned int key = irq_lock();

	if (!f->outstanding) {
		irq_unlock(key);
		return;
	}
	f->outstanding = false;
	irq_unlock(key);

	uint32_t est = fake_cost_of_us(fake_devs[f->idx], f->len);
	uint32_t pct = f->cfg.airtime_pct != 0U ? f->cfg.airtime_pct : 100U;
	uint32_t airtime = (uint32_t)((uint64_t)est * pct / 100U);
	int status = 0;

	if (f->cfg.caps.half_duplex) {
		airtime += f->cfg.rx_turnaround_us;
	}
	if (f->cfg.loss_pct != 0U && (rnd(f) % 100U) < f->cfg.loss_pct) {
		status = -EIO;
	}
	if (status == 0) {
		f->ctr.tx_ok++;
		f->ctr.bytes_ok += f->len;
	} else {
		f->ctr.tx_fail++;
	}
	if (tx_cb != NULL) {
		tx_cb(f->idx, f->buf, f->len, status);
	}
	dm_link_tx_done(f->link, f->handle, status, airtime);
}

static void tx_timer_fn(struct k_timer *t)
{
	complete(CONTAINER_OF(t, struct dm_fake, tx_timer));
}

static int fake_tx(const struct device *dev, const uint8_t *buf, size_t len, uint8_t mac_prio,
		   uint32_t *handle)
{
	struct dm_fake *f = fake_of(dev);

	f->ctr.tx_calls++;
	f->ctr.last_mac_prio = mac_prio;
	if (f->cfg.down) {
		return -ENETDOWN;
	}
	if (f->outstanding) {
		f->ctr.overlap++;
	}
	f->outstanding = true;
	f->handle = ++f->next_handle;
	f->buf = buf;
	f->len = len;
	*handle = f->handle;

	if (f->cfg.drop_completion) {
		return 0;
	}
	uint32_t delay = f->cfg.airtime_latency ? fake_cost_of_us(dev, len) : f->cfg.latency_us;

	if (delay == 0U) {
		complete(f);
	} else {
		k_timer_start(&f->tx_timer, K_USEC(delay), K_NO_WAIT);
	}
	return 0;
}

static int fake_cancel(const struct device *dev, uint32_t handle)
{
	struct dm_fake *f = fake_of(dev);

	ARG_UNUSED(handle);
	k_timer_stop(&f->tx_timer);
	f->outstanding = false;
	f->ctr.cancels++;
	return 0;
}

static void probe_timer_fn(struct k_timer *t)
{
	struct dm_fake *f = CONTAINER_OF(t, struct dm_fake, probe_timer);

	dm_link_probe_done(f->link, f->cfg.probe_rtt_us, f->cfg.probe_fail ? 100U : 0U);
}

static int fake_probe(const struct device *dev)
{
	struct dm_fake *f = fake_of(dev);

	f->ctr.probes++;
	if (f->cfg.down) {
		return -ENETDOWN;
	}
	k_timer_start(&f->probe_timer, K_USEC(MAX(f->cfg.probe_rtt_us, 1U)), K_NO_WAIT);
	return 0;
}

static bool fake_is_busy(const struct device *dev)
{
	struct dm_fake *f = fake_of(dev);

	f->ctr.busy_checks++;
	if (f->cfg.busy_checks != 0U) {
		f->cfg.busy_checks--;
		return true;
	}
	return false;
}

static const struct dm_link_ops fake_ops = {
	.tx = fake_tx,
	.cancel = fake_cancel,
	.cost_of_us = fake_cost_of_us,
	.probe = fake_probe,
	.is_busy = fake_is_busy,
};

const struct device *dm_fake_get(uint8_t i)
{
	return i < CONFIG_DM_LINK_FAKE_COUNT ? fake_devs[i] : NULL;
}

void dm_fake_configure(uint8_t i, const struct dm_fake_cfg *cfg)
{
	fakes[i].cfg = *cfg;
	fakes[i].rng = cfg->seed;
}

struct dm_fake_cfg *dm_fake_cfg(uint8_t i)
{
	return &fakes[i].cfg;
}

int dm_fake_register(uint8_t i, uint8_t *out_link)
{
	struct dm_fake *f = &fakes[i];
	int rc;

	f->idx = i;
	k_timer_init(&f->tx_timer, tx_timer_fn, NULL);
	k_timer_init(&f->probe_timer, probe_timer_fn, NULL);
	rc = dm_link_register(fake_devs[i], &fake_ops, &f->cfg.caps, &f->link);
	if (rc == 0) {
		f->registered = true;
		*out_link = f->link;
	}
	return rc;
}

void dm_fake_set_tx_cb(dm_fake_tx_cb_t cb)
{
	tx_cb = cb;
}

void dm_fake_counters(uint8_t i, struct dm_fake_counters *out)
{
	*out = fakes[i].ctr;
}

void dm_fake_reset_all(void)
{
	for (int i = 0; i < CONFIG_DM_LINK_FAKE_COUNT; i++) {
		if (fakes[i].registered) {
			k_timer_stop(&fakes[i].tx_timer);
			k_timer_stop(&fakes[i].probe_timer);
		}
		memset(&fakes[i], 0, sizeof(fakes[i]));
		fakes[i].idx = (uint8_t)i;
	}
	tx_cb = NULL;
}
