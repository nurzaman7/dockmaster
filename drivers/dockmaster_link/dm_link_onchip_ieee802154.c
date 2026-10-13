/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-chip IEEE 802.15.4 radio (spec section 9): a dockmaster,link node
 * whose backend is a radio implementing Zephyr's ieee802154_radio_api.
 *
 * DM_TIER_MAC with two levels, mapped onto the API's TX modes:
 *   mac_prio 0 (C0 to C2): IEEE802154_TX_MODE_DIRECT, no channel assessment,
 *   mac_prio 1 (C3, C4):   IEEE802154_TX_MODE_CSMA_CA when the radio offloads
 *                          CSMA-CA (IEEE802154_HW_CSMA), else TX_MODE_CCA.
 * On a shared channel the upper classes therefore never back off while
 * bulk traffic does: the radio-native equivalent of a shorter AIFS.
 *
 * Each Dockmaster message becomes one data frame with a 9-byte header
 * built here (short addresses, PAN ID compression, from Kconfig). The
 * radio appends the FCS, as Zephyr's radio API expects. The frame needs a
 * net_pkt, so the application must enable the net_pkt pools; the minimum
 * is CONFIG_NETWORKING, CONFIG_IEEE802154 and CONFIG_IEEE802154_RAW_MODE.
 * In raw mode Dockmaster is the radio's only transmitter, and the
 * application provides net_recv_data() and ieee802154_handle_ack(), which
 * the radio driver calls for received frames and ACKs (reception is
 * outside Dockmaster). With a full L2 nothing else may transmit on the
 * radio, because radio drivers are not reentrant.
 */

#define DT_DRV_COMPAT dockmaster_link

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/sys/byteorder.h>

#include <dockmaster/dm_link.h>

LOG_MODULE_REGISTER(dm_ieee802154, CONFIG_DM_LOG_LEVEL);

/* 2.4 GHz O-QPSK PHY: 250 kbit/s, 32 us per octet. */
#define US_PER_OCTET  32U
#define PHY_HDR       6U   /* preamble 4, SFD 1, PHR 1 */
#define MHR_LEN       9U   /* FCF 2, sequence 1, PAN ID 2, destination 2, source 2 */
#define FCS_LEN       2U
#define MAX_PSDU      127U /* aMaxPhyPacketSize */
#define LINK_MTU      (MAX_PSDU - MHR_LEN - FCS_LEN)
#define CCA_US        128U /* 8 symbols */
#define TURNAROUND_US 192U /* aTurnaroundTime, 12 symbols */
#define ACK_US        ((PHY_HDR + 5U) * US_PER_OCTET)
#define SHORT_BCAST   0xFFFFU

/* Frame control: data frame, PAN ID compression, short addresses, 2003 version. */
#define FCF_DATA   0x0001U
#define FCF_AR     BIT(5)
#define FCF_PANC   BIT(6)
#define FCF_DST_16 (2U << 10)
#define FCF_SRC_16 (2U << 14)

struct r154_cfg {
	const struct device *radio;
	struct dm_link_caps caps;
};

struct r154_data {
	enum ieee802154_tx_mode low_mode; /* TX mode of mac_prio 1 */
	bool ack;                         /* frames ask for an ACK */
	uint8_t seq;
	uint8_t link;
	uint32_t handle;
};

static void mhr_put(uint8_t *p, uint8_t seq, bool ack)
{
	sys_put_le16(FCF_DATA | FCF_PANC | FCF_DST_16 | FCF_SRC_16 | (ack ? FCF_AR : 0U), &p[0]);
	p[2] = seq;
	sys_put_le16(CONFIG_DM_LINK_IEEE802154_PAN_ID, &p[3]);
	sys_put_le16(CONFIG_DM_LINK_IEEE802154_DST_ADDR, &p[5]);
	sys_put_le16(CONFIG_DM_LINK_IEEE802154_SHORT_ADDR, &p[7]);
}

/* Pure: reads only the ack flag fixed at init; 32-bit math. */
static uint32_t r154_cost_of_us(const struct device *dev, size_t len)
{
	const struct r154_data *d = dev->data;
	uint32_t us = (PHY_HDR + MHR_LEN + (uint32_t)len + FCS_LEN) * US_PER_OCTET + CCA_US +
		      TURNAROUND_US;

	return d->ack ? us + TURNAROUND_US + ACK_US : us;
}

/*
 * Zephyr's radio tx() is synchronous: it returns once the frame is out (and
 * acknowledged, when it asked for an ACK) or has failed. The completion is
 * therefore reported here with dm_link_tx_done(), before ops->tx returns,
 * which the dm_link contract allows. The measured time includes CCA,
 * backoff and the ACK wait, i.e. how long the link was unusable, so it is
 * also reported for failures (-EBUSY channel busy, -ENOMSG no ACK).
 */
static int r154_tx(const struct device *dev, const uint8_t *buf, size_t len, uint8_t mac_prio,
		   uint32_t *handle)
{
	const struct r154_cfg *cfg = dev->config;
	const struct ieee802154_radio_api *api = cfg->radio->api;
	struct r154_data *d = dev->data;
	enum ieee802154_tx_mode mode = mac_prio == 0U ? IEEE802154_TX_MODE_DIRECT : d->low_mode;
	struct net_pkt *pkt;
	struct net_buf *frag;
	uint32_t t0;
	int rc;

	if (len == 0U || len > LINK_MTU) {
		return -EMSGSIZE;
	}
	pkt = net_pkt_alloc_with_buffer(NULL, MHR_LEN + len, NET_AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		return -ENOMEM;
	}
	frag = pkt->buffer;
	if (frag == NULL || net_buf_tailroom(frag) < MHR_LEN + len) {
		net_pkt_unref(pkt);
		return -ENOMEM; /* the frame must fit one net_buf */
	}
	mhr_put(net_buf_add(frag, MHR_LEN), d->seq++, d->ack);
	net_buf_add_mem(frag, buf, len);

	*handle = ++d->handle;
	t0 = k_cycle_get_32();
	rc = api->tx(cfg->radio, mode, pkt, frag);
	net_pkt_unref(pkt);
	dm_link_tx_done(d->link, *handle, rc, k_cyc_to_us_floor32(k_cycle_get_32() - t0));
	return 0;
}

/* tx() completes before it returns, so there is never anything to cancel. */
static int r154_cancel(const struct device *dev, uint32_t handle)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(handle);
	return 0;
}

static const struct dm_link_ops r154_ops = {
	.tx = r154_tx,
	.cancel = r154_cancel,
	.cost_of_us = r154_cost_of_us,
};

static int r154_init(const struct device *dev)
{
	const struct r154_cfg *cfg = dev->config;
	struct r154_data *d = dev->data;
	const struct ieee802154_radio_api *api;
	struct ieee802154_filter f;
	enum ieee802154_hw_caps hw;
	int rc;

	if (!device_is_ready(cfg->radio)) {
		LOG_ERR("%s: radio not ready", dev->name);
		return -ENODEV;
	}
	api = cfg->radio->api;
	hw = api->get_capabilities(cfg->radio);
	d->low_mode = (hw & IEEE802154_HW_CSMA) != 0 ? IEEE802154_TX_MODE_CSMA_CA
						      : IEEE802154_TX_MODE_CCA;
	d->ack = CONFIG_DM_LINK_IEEE802154_DST_ADDR != SHORT_BCAST &&
		 (hw & IEEE802154_HW_TX_RX_ACK) != 0;

	rc = api->set_channel(cfg->radio, CONFIG_DM_LINK_IEEE802154_CHANNEL);
	if (rc == 0 && (hw & IEEE802154_HW_FILTER) != 0) {
		f.pan_id = CONFIG_DM_LINK_IEEE802154_PAN_ID;
		rc = api->filter(cfg->radio, true, IEEE802154_FILTER_TYPE_PAN_ID, &f);
		if (rc == 0) {
			f.short_addr = CONFIG_DM_LINK_IEEE802154_SHORT_ADDR;
			rc = api->filter(cfg->radio, true, IEEE802154_FILTER_TYPE_SHORT_ADDR, &f);
		}
	}
	if (rc == 0) {
		rc = api->start(cfg->radio);
		rc = rc == -EALREADY ? 0 : rc;
	}
	if (rc == 0) {
		rc = dm_link_register(dev, &r154_ops, &cfg->caps, &d->link);
	}
	if (rc != 0) {
		LOG_ERR("%s: init failed (%d)", dev->name, rc);
		return rc;
	}
	LOG_INF("%s: channel %u, level 1 %s, %s", dev->name, CONFIG_DM_LINK_IEEE802154_CHANNEL,
		d->low_mode == IEEE802154_TX_MODE_CSMA_CA ? "CSMA-CA" : "CCA",
		d->ack ? "acked" : "no ACK");
	return 0;
}

#define R154_TIER(n) DT_INST_ENUM_IDX_OR(n, tier, DM_TIER_MAC)

#define R154_DEFINE(n)                                                                      \
	BUILD_ASSERT(R154_TIER(n) != DM_TIER_HW,                                            \
		     "dockmaster,link (802.15.4): tier must be sw or mac");                 \
	static struct r154_data r154_data_##n;                                              \
	static const struct r154_cfg r154_cfg_##n = {                                       \
		.radio = DEVICE_DT_GET(DT_INST_PHANDLE(n, backend)),                        \
		.caps = {                                                                   \
			.mtu = LINK_MTU,                                                    \
			.tier = R154_TIER(n),                                               \
			.mac_prio_levels = 2,                                               \
			.half_duplex = true,                                                \
			.metered = DT_INST_PROP(n, metered),                                \
			.owner_prio = DT_INST_PROP_OR(n, owner_priority, 0),                \
			.owner_stack = DT_INST_PROP_OR(n, owner_stack, 0),                  \
		},                                                                          \
	};                                                                                  \
	DEVICE_DT_INST_DEFINE(n, r154_init, NULL, &r154_data_##n, &r154_cfg_##n,            \
			      POST_KERNEL, CONFIG_DM_LINK_IEEE802154_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(R154_DEFINE)
