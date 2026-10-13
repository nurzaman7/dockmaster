.. _dockmaster_ncp_uart_link:

Dockmaster: co-processor link on UART
#####################################

Overview
********

A network co-processor (for example a cellular modem) on USART1 of a
NUCLEO-L476RG, driven by ``dm_link_ncp_uart.c``, next to a local radio that
a fake link stands in for. The STM32L4 has 4 NVIC priority bits, so this
sample is the build target for the 4-bit priority path of the spec.

The co-processor protocol (``AT+SEND``, ``AT+PING``, ``AT+ABORT``, replies
``OK <id>`` and ``ERROR <id>``) is documented at the top of the driver.

This sample builds in CI. It has not been run on hardware yet, and no
co-processor implementing the protocol is part of this repository.

Wiring
******

======  =====  =======  ============
Signal  Pin    Arduino  Co-processor
======  =====  =======  ============
TX      PA9    D8       RX
RX      PA10   D2       TX
GND     GND    GND      GND
======  =====  =======  ============

115200 baud, 8N1, 3.3 V. The console stays on the ST-LINK virtual COM port.

Building
********

.. code-block:: console

   west build -b nucleo_l476rg dockmaster/samples/ncp_uart_link

What to expect
**************

Every 5 s the sample prints per-class and per-link counters. With the
default policy C4 (bulk) never uses the metered co-processor link and C0
(control) goes out on both links. With nothing connected to USART1 the
co-processor link misses its replies, collects watchdog faults and goes
down, and all traffic stays on the local radio.
