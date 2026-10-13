.. Copyright (c) 2026 Nurzaman
.. SPDX-License-Identifier: Apache-2.0

Dockmaster
##########

Firmware QoS for multi-radio Cortex-M devices on Zephyr: one traffic-class
table, one airtime scheduler and one link selector, enforced from thread
priorities down to NVIC priorities.

.. toctree::
   :maxdepth: 2
   :caption: Contents

   architecture
   api
   testing
   deviations
   spec

.. toctree::
   :maxdepth: 1
   :caption: Decisions

   adr/index
   adr/0001-airtime-units
   adr/0002-single-scheduler-thread
   adr/0003-tx-only-tier0
   adr/0004-class-queue-rings
