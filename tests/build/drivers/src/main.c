/*
 * Copyright (c) 2026 Nurzaman
 * SPDX-License-Identifier: Apache-2.0
 *
 * Build-only test: every reference link driver and the nRF tier 0 backend
 * must compile and link for real hardware targets. The asserts fail the
 * build if a driver silently drops out because a dependency is missing.
 */

#include <zephyr/kernel.h>

#include <dockmaster/dm_link.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_DM_LINK_SPI_PHY), "SPI PHY driver not built");
BUILD_ASSERT(IS_ENABLED(CONFIG_DM_LINK_NCP_UART), "NCP UART driver not built");
BUILD_ASSERT(IS_ENABLED(CONFIG_DM_TIER_HW_NRF), "nRF tier 0 backend not built");
BUILD_ASSERT(!DT_HAS_COMPAT_STATUS_OKAY(dockmaster_link) || IS_ENABLED(CONFIG_DM_LINK_IEEE802154),
	     "IEEE 802.15.4 driver not built");

#if defined(CONFIG_IEEE802154_RAW_MODE)
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/net_pkt.h>

/*
 * Raw mode: the radio driver hands received frames and ACKs to these two
 * functions instead of a net stack. Reception is outside Dockmaster, so
 * this test just drops them.
 */
int net_recv_data(struct net_if *iface, struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	net_pkt_unref(pkt);
	return 0;
}

enum net_verdict ieee802154_handle_ack(struct net_if *iface, struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(pkt);
	return NET_CONTINUE;
}
#endif

int main(void)
{
	printk("dockmaster drivers: %u link(s) registered\n", dm_link_count());
	return 0;
}
