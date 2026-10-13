# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0

zephyr_sources_ifdef(CONFIG_DM_LINK_FAKE dm_link_fake.c)
zephyr_sources_ifdef(CONFIG_DM_LINK_SPI_PHY dm_link_spi_phy.c)
zephyr_sources_ifdef(CONFIG_DM_LINK_NCP_UART dm_link_ncp_uart.c)
zephyr_sources_ifdef(CONFIG_DM_LINK_IEEE802154 dm_link_onchip_ieee802154.c)
