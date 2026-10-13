/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network co-processor on a UART (spec section 9): the radio and its MAC
 * live on another chip, a cellular or LPWAN modem for example, driven by a
 * small line-oriented, AT-like protocol. Lines end in "\r\n"; <id> is four
 * upper-case hex digits chosen by the host, never 0000:
 *
 *   host -> NCP                     NCP -> host
 *   AT+SEND=<id>,<hex payload>      "OK <id>" once it has sent the payload,
 *                                   "ERROR <id>" if it gave up
 *   AT+PING=<id>                    "OK <id>" (round-trip probe)
 *   AT+ABORT                        nothing; drops any SEND in progress
 *
 * Every other line from the NCP (unsolicited result codes, for example) is
 * ignored. The payload travels as hex so a line can never contain a stray
 * "\r\n" and every byte on the wire is printable.
 *
 * Assumptions, untested on hardware: echo is off, the NCP answers each
 * SEND and PING exactly once, and it accepts lines of 2 * mtu + 15 chars.
 *
 * The link is metered and DM_TIER_SW with no is_busy: the co-processor runs
 * its own channel access. Its airtime is the whole request/response time,
 * the figure the link manager should compare against other links. RX and TX
 * are interrupt driven; the ISR only moves bytes and, on a complete reply
 * line, reports the completion.
 */

#define DT_DRV_COMPAT dockmaster_ncp_uart

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <dockmaster/dm_link.h>

LOG_MODULE_REGISTER(dm_ncp_uart, CONFIG_DM_LOG_LEVEL);

#define SEND_PREFIX  "AT+SEND="
#define PING_PREFIX  "AT+PING="
#define ABORT_LINE   "\r\nAT+ABORT\r\n" /* leading CRLF ends a cut-off line */
#define SEND_OVERHEAD 15U               /* "AT+SEND=" id "," ... "\r\n" */
#define REPLY_CHARS  9U                 /* "OK <id>\r\n" */
#define RX_LINE_MAX  32U

struct ncp_cfg {
	const struct device *uart;
	uint8_t *tx_buf;
	struct dm_link_caps caps;
	uint32_t us_per_char_q8; /* 10 bits per character, in 1/256 us */
	uint32_t latency_us;
};

struct ncp_data {
	uint8_t link;
	uint16_t next_id;
	/* Owner thread to ISR. */
	size_t tx_len;
	size_t tx_pos;
	atomic_t tx_active;
	atomic_t send_id; /* SEND waiting for its reply, 0 = none */
	atomic_t ping_id; /* PING waiting for its reply, 0 = none */
	uint32_t send_t0;
	uint32_t ping_t0;
	/* ISR only. */
	char line[RX_LINE_MAX];
	uint8_t line_len;
	bool overflow;
};

static const char hex_digit[] = "0123456789ABCDEF";

static size_t put_hex(uint8_t *out, const uint8_t *in, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = hex_digit[in[i] >> 4];
		out[2 * i + 1] = hex_digit[in[i] & 0x0FU];
	}
	return 2U * n;
}

static size_t put_id(uint8_t *out, uint16_t id)
{
	const uint8_t be[] = {(uint8_t)(id >> 8), (uint8_t)id};

	return put_hex(out, be, sizeof(be));
}

/* Exactly four hex digits, then the end of the line. */
static int parse_id(const char *s, uint16_t *id)
{
	uint16_t v = 0U;

	for (int i = 0; i < 4; i++) {
		const char *p = strchr(hex_digit, s[i]);

		if (s[i] == '\0' || p == NULL) {
			return -EINVAL;
		}
		v = (uint16_t)((v << 4) | (uint16_t)(p - hex_digit));
	}
	*id = v;
	return (s[4] == '\0' && v != 0U) ? 0 : -EINVAL;
}

static uint16_t ncp_next_id(struct ncp_data *d)
{
	d->next_id = d->next_id == 0xFFFFU ? 1U : d->next_id + 1U;
	return d->next_id;
}

static void ncp_start(const struct ncp_cfg *cfg, struct ncp_data *d, size_t len)
{
	d->tx_len = len;
	d->tx_pos = 0U;
	atomic_set(&d->tx_active, 1);
	uart_irq_tx_enable(cfg->uart);
}

/* ISR: one complete line from the NCP. */
static void ncp_line(struct ncp_data *d)
{
	const char *s = d->line;
	uint32_t now = k_cycle_get_32();
	uint16_t id;
	int status;

	if (strncmp(s, "OK ", 3) == 0) {
		status = 0;
		s += 3;
	} else if (strncmp(s, "ERROR ", 6) == 0) {
		status = -EIO;
		s += 6;
	} else {
		LOG_DBG("line ignored");
		return;
	}
	if (parse_id(s, &id) != 0) {
		LOG_DBG("malformed reply");
		return;
	}
	if (atomic_cas(&d->send_id, id, 0)) {
		dm_link_tx_done(d->link, id, status, k_cyc_to_us_floor32(now - d->send_t0));
	} else if (atomic_cas(&d->ping_id, id, 0)) {
		dm_link_probe_done(d->link, k_cyc_to_us_floor32(now - d->ping_t0),
				   status == 0 ? 0U : 100U);
	} else {
		LOG_DBG("stray reply %04x", id);
	}
}

static void ncp_rx_byte(struct ncp_data *d, uint8_t c)
{
	if (c == '\n') {
		if (!d->overflow) {
			d->line[d->line_len] = '\0';
			ncp_line(d);
		}
		d->line_len = 0U;
		d->overflow = false;
	} else if (c != '\r') {
		if (d->line_len < RX_LINE_MAX - 1U) {
			d->line[d->line_len++] = (char)c;
		} else {
			d->overflow = true; /* too long to be a reply */
		}
	}
}

static void ncp_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	const struct ncp_cfg *cfg = dev->config;
	struct ncp_data *d = dev->data;
	uint8_t buf[16];
	int n;

	if (!uart_irq_update(uart)) {
		return;
	}
	if (uart_irq_rx_ready(uart)) {
		while ((n = uart_fifo_read(uart, buf, sizeof(buf))) > 0) {
			for (int i = 0; i < n; i++) {
				ncp_rx_byte(d, buf[i]);
			}
		}
	}
	if (atomic_get(&d->tx_active) != 0 && uart_irq_tx_ready(uart)) {
		if (d->tx_pos < d->tx_len) {
			n = uart_fifo_fill(uart, &cfg->tx_buf[d->tx_pos], d->tx_len - d->tx_pos);
			d->tx_pos += n > 0 ? (size_t)n : 0U;
		}
		if (d->tx_pos >= d->tx_len) {
			uart_irq_tx_disable(uart);
			atomic_set(&d->tx_active, 0);
		}
	}
}

/* Pure: configuration only, 32-bit math (mtu <= 255, baud >= 1200). */
static uint32_t ncp_cost_of_us(const struct device *dev, size_t len)
{
	const struct ncp_cfg *cfg = dev->config;
	uint32_t chars = SEND_OVERHEAD + 2U * (uint32_t)len + REPLY_CHARS;

	return cfg->latency_us + ((chars * cfg->us_per_char_q8 + 255U) >> 8);
}

static int ncp_tx(const struct device *dev, const uint8_t *buf, size_t len, uint8_t mac_prio,
		  uint32_t *handle)
{
	const struct ncp_cfg *cfg = dev->config;
	struct ncp_data *d = dev->data;
	uint8_t *out = cfg->tx_buf;
	size_t n = sizeof(SEND_PREFIX) - 1U;
	uint16_t id;

	ARG_UNUSED(mac_prio);
	if (len == 0U || len > cfg->caps.mtu) {
		return -EMSGSIZE;
	}
	if (atomic_get(&d->tx_active) != 0) {
		return -EBUSY; /* an ABORT line is still going out */
	}
	id = ncp_next_id(d);
	memcpy(out, SEND_PREFIX, n);
	n += put_id(&out[n], id);
	out[n++] = ',';
	n += put_hex(&out[n], buf, len);
	out[n++] = '\r';
	out[n++] = '\n';

	d->send_t0 = k_cycle_get_32();
	atomic_set(&d->send_id, id);
	*handle = id;
	ncp_start(cfg, d, n);
	return 0;
}

static int ncp_cancel(const struct device *dev, uint32_t handle)
{
	const struct ncp_cfg *cfg = dev->config;
	struct ncp_data *d = dev->data;

	ARG_UNUSED(handle);
	atomic_set(&d->send_id, 0); /* a late OK or ERROR is now ignored */
	uart_irq_tx_disable(cfg->uart);
	atomic_set(&d->tx_active, 0);
	memcpy(cfg->tx_buf, ABORT_LINE, sizeof(ABORT_LINE) - 1U);
	ncp_start(cfg, d, sizeof(ABORT_LINE) - 1U);
	return 0;
}

static int ncp_probe(const struct device *dev)
{
	const struct ncp_cfg *cfg = dev->config;
	struct ncp_data *d = dev->data;
	size_t n = sizeof(PING_PREFIX) - 1U;
	uint16_t id;

	if (atomic_get(&d->tx_active) != 0) {
		return -EBUSY;
	}
	id = ncp_next_id(d);
	memcpy(cfg->tx_buf, PING_PREFIX, n);
	n += put_id(&cfg->tx_buf[n], id);
	cfg->tx_buf[n++] = '\r';
	cfg->tx_buf[n++] = '\n';

	d->ping_t0 = k_cycle_get_32();
	atomic_set(&d->ping_id, id);
	ncp_start(cfg, d, n);
	return 0;
}

static const struct dm_link_ops ncp_ops = {
	.tx = ncp_tx,
	.cancel = ncp_cancel,
	.cost_of_us = ncp_cost_of_us,
	.probe = ncp_probe,
};

static int ncp_init(const struct device *dev)
{
	const struct ncp_cfg *cfg = dev->config;
	struct ncp_data *d = dev->data;
	int rc;

	if (!device_is_ready(cfg->uart)) {
		LOG_ERR("%s: UART not ready", dev->name);
		return -ENODEV;
	}
	rc = uart_irq_callback_user_data_set(cfg->uart, ncp_isr, (void *)dev);
	if (rc == 0) {
		rc = dm_link_register(dev, &ncp_ops, &cfg->caps, &d->link);
	}
	if (rc != 0) {
		LOG_ERR("%s: init failed (%d)", dev->name, rc);
		return rc;
	}
	uart_irq_rx_enable(cfg->uart);
	return 0;
}

#define NCP_BAUD(n) DT_PROP_OR(DT_INST_BUS(n), current_speed, 115200)

#define NCP_DEFINE(n)                                                                       \
	BUILD_ASSERT(DT_INST_PROP(n, mtu) >= 1 && DT_INST_PROP(n, mtu) <= 255,              \
		     "dockmaster,ncp-uart: mtu must be 1 to 255");                          \
	BUILD_ASSERT(NCP_BAUD(n) >= 1200, "dockmaster,ncp-uart: baud rate below 1200");     \
	BUILD_ASSERT(DT_INST_ENUM_IDX_OR(n, tier, DM_TIER_SW) == DM_TIER_SW,                \
		     "dockmaster,ncp-uart: links are tier sw");                             \
	static uint8_t ncp_tx_buf_##n[SEND_OVERHEAD + 2U * DT_INST_PROP(n, mtu)];           \
	static struct ncp_data ncp_data_##n;                                                \
	static const struct ncp_cfg ncp_cfg_##n = {                                         \
		.uart = DEVICE_DT_GET(DT_INST_BUS(n)),                                      \
		.tx_buf = ncp_tx_buf_##n,                                                   \
		.caps = {                                                                   \
			.mtu = DT_INST_PROP(n, mtu),                                        \
			.tier = DM_TIER_SW,                                                 \
			.metered = true,                                                    \
			.probe_period_ms = DT_INST_PROP(n, probe_period_ms),                \
			.owner_prio = DT_INST_PROP_OR(n, owner_priority, 0),                \
			.owner_stack = DT_INST_PROP_OR(n, owner_stack, 0),                  \
		},                                                                          \
		.us_per_char_q8 = (uint32_t)(2560000000ULL / NCP_BAUD(n)),                  \
		.latency_us = DT_INST_PROP(n, latency_us),                                  \
	};                                                                                  \
	DEVICE_DT_INST_DEFINE(n, ncp_init, NULL, &ncp_data_##n, &ncp_cfg_##n, POST_KERNEL,  \
			      CONFIG_DM_LINK_NCP_UART_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(NCP_DEFINE)
