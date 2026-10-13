# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_DOCKMASTER)
  zephyr_include_directories(include)
  add_subdirectory(subsys/dockmaster)
  add_subdirectory(drivers/dockmaster_link)
endif()
