# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(dockmaster)

zephyr_library_sources(
  dm_core.c
  dm_queue.c
  dm_sched.c
  dm_owner.c
  dm_linkmgr.c
  dm_policy.c
  dm_policy_default.c
  dm_pressure.c
  dm_stats.c
  dm_prio.c
)
zephyr_library_sources_ifdef(CONFIG_DM_SHELL dm_shell.c)

if(CONFIG_DM_TIER_HW_NRF)
  zephyr_library_sources(hw/dm_hw_nrf_egu.c)
else()
  zephyr_library_sources(hw/dm_hw_none.c)
endif()

# Spec section 15: -Wall -Wextra -Werror for everything under subsys/dockmaster.
# -Wtype-limits is off because Zephyr's own time_units.h trips it on 64-bit
# hosts; the other two are idioms Zephyr code uses throughout.
zephyr_library_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-missing-field-initializers -Wno-type-limits)
