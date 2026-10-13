.. _dockmaster_dk_spi_phy_edca:

Dockmaster: SPI PHY hardware labs (nRF52840 DK)
###############################################

.. note::

   None of the labs below has been run yet. This page is the procedure,
   not a report: it claims no numbers. The sample builds in CI for
   ``nrf52840dk/nrf52840`` in all three variants listed under Building.

Overview
********

One external PHY on ``spi1`` of an nRF52840 DK, driven by the generic
``dockmaster,spi-phy`` link driver (command set documented at the top of
``drivers/dockmaster_link/dm_link_spi_phy.c``). ``main()`` keeps a C4 (bulk)
producer running at a fixed period, injects one C0 (control) message per
second, and every 10 s prints the Dockmaster counters and the thread
analyzer's stack high-water marks.

The build enables the three things the labs in section 15 of the spec need:

- ``CONFIG_DM_GPIO_PROBES``: pin A toggles when a link reports a completion
  from interrupt context (``dm_link_tx_done()``), pin B toggles when an
  owner thread calls the driver's ``tx``.
- ``CONFIG_DM_TIER_HW_NRF`` with a ``dockmaster,hw-nrf-egu`` node: tier 0,
  class line 0 (C0 to C2) on EGU2 at NVIC priority 1, class line 1 (C3, C4)
  on EGU3 at NVIC priority 3.
- ``CONFIG_THREAD_ANALYZER``: stack usage of the scheduler and owner threads.

Requirements
************

- nRF52840 DK.
- An SPI transceiver that implements the assumed command set (or a small
  adapter MCU that translates it), with a done output, a busy output and
  two more outputs that can be mapped to the done event. It must run at
  3.3 V logic.
- A logic analyser with at least three channels.
- For lab 2: a second DK with an identical PHY, and a receiver for the PHY's
  modulation that counts good frames and CRC failures. Neither the
  receiver nor its firmware is part of this repository.

Wiring
******

All signals are on the DK's Arduino header.

=====================  =====  =======  ==========================
Signal                 Pin    Header   Notes
=====================  =====  =======  ==========================
PHY SCK                P0.31  A5       spi1 default pinctrl
PHY MOSI               P0.30  A4       spi1 default pinctrl
PHY MISO               P1.08  D7       spi1 default pinctrl
PHY CS                 P1.07  D6       active low
PHY done (output 0)    P1.03  D2       tier "sw" completion
PHY busy               P1.04  D3       high while channel busy
PHY output 1           P1.05  D4       class line 0, C0 to C2
PHY output 2           P1.06  D5       class line 1, C3 and C4
Probe A                P0.03  A0       logic analyser channel 1
Probe B                P0.04  A1       logic analyser channel 2
Lab 3 stimulus         P0.28  A2       only with ``lab3.overlay``
=====================  =====  =======  ==========================

Building
********

.. code-block:: console

   # Tier 0 (default): labs 1 and 3
   west build -b nrf52840dk/nrf52840 dockmaster/samples/dk_spi_phy_edca

   # Tier "sw" with sw-EDCA on the busy pin: lab 2
   west build -b nrf52840dk/nrf52840 dockmaster/samples/dk_spi_phy_edca -- \
     -DEXTRA_DTC_OVERLAY_FILE=edca.overlay

   # Lab 3 stimulus, with CTF tracing into a RAM buffer
   west build -b nrf52840dk/nrf52840 dockmaster/samples/dk_spi_phy_edca -- \
     -DEXTRA_DTC_OVERLAY_FILE=lab3.overlay -DCONFIG_TRACING=y \
     -DCONFIG_TRACING_CTF=y -DCONFIG_TRACING_BACKEND_RAM=y -DCONFIG_DM_TRACING=y

The producer periods are Kconfig options of the sample
(``CONFIG_APP_C4_PERIOD_MS``, ``CONFIG_APP_C4_LEN``,
``CONFIG_APP_C0_PERIOD_MS``, ``CONFIG_APP_REPORT_PERIOD_S``).

For every run, record the git commit, the exact build command and the
console output (counters and thread analyzer report).

Lab 1: interrupt-to-tx latency
******************************

What it measures: the time from a completion interrupt to the next ``tx``
call on the same link, through the whole pipeline (``dm_link_tx_done()``,
owner thread, scheduler round, dispatch, owner thread). This is the number
behind the priority table in section 11 of the spec.

Procedure:

#. Default build. Keep C4 backlogged (the default 20 ms period with a
   250 kbit/s PHY is enough) so that every completion is followed by a tx.
#. Capture probe A and probe B at 25 MS/s or more for at least 10 s, and
   export the capture as CSV (time, A, B).
#. Run ``python3 scripts/lab_latency.py capture.csv`` from the repository
   root. It pairs every A edge with the next B edge and prints p50, p99,
   min and max.
#. Repeat with a vendor radio stack advertising every 20 ms. The sample
   does not include a Bluetooth configuration yet; adding one (for example
   Zephyr's controller and a connectable advertiser) is part of running
   this lab.

Record: p50, p99, min, max and the number of pairs for both runs, and the
CSV files.

Pass criteria (from the spec, not yet observed): tens of microseconds with
the radio stack idle; at most about one connection event with the stack
busy.

Note: pin A toggles inside ``dm_link_tx_done()``, a few microseconds after
the hardware event. To include that part, also capture the PHY's done or
class-line output and measure from its edge instead.

Lab 2: sw-EDCA collisions
*************************

What it measures: whether software EDCA on the busy pin reduces collisions
between two uncoordinated transmitters, and how long C0 waits during a C4
storm.

Procedure:

#. Two DKs, each with a PHY on the same channel, both flashed with the
   ``edca.overlay`` build and the same ``CONFIG_APP_C4_PERIOD_MS``. A third
   receiver counts good frames and CRC failures.
#. Baseline: build with ``-DCONFIG_DM_EDCA=n``, run both boards for 10
   minutes, and record frames received and CRC failures.
#. Repeat with ``CONFIG_DM_EDCA=y`` (the default).
#. C0 wait: in the EDCA run, build one board with the CTF tracing options
   shown under Building and measure, for each C0 message handle, the time
   from the ``dm_submit`` event to its ``dm_owner_tx`` event.

The default C4 EDCA parameters (50 ms AIFS, contention window of 0 to
400 ms) limit C4 to a few frames per second. To compare collision rates at
the same offered load, choose a C4 period longer than the mean EDCA wait,
or lower ``CONFIG_DM_EDCA_AIFS_US_C4`` and ``CONFIG_DM_EDCA_CW_US_C4``, and
use the same values for both runs. The console's ``edca_wait`` counter is
the cumulative EDCA wait per link.

Record: CRC failures per 1000 frames with and without EDCA, the C4 period
and EDCA settings used, and the median and p99 C0 wait.

Pass criteria (from the spec, not yet observed): collisions drop by about
an order of magnitude with EDCA; the median C0 wait stays below the C0
contention window, 0 to 8 ms by default (``CONFIG_DM_EDCA_CW_US_C0``;
on a busy medium the core doubles it, up to four times that).

Lab 3: tier 0 ordering
**********************

What it measures: that a C0 to C2 completion (class line 0, EGU2, NVIC
priority 1) is always handled before a C3 or C4 completion (class line 1,
EGU3, NVIC priority 3) that becomes pending at the same time, and that the
class lines never raise the GPIOTE interrupt.

Procedure:

#. Use the ``lab3.overlay`` build with tracing. Disconnect the PHY's
   outputs 1 and 2 from D4 and D5 and jumper A2 (P0.28) to both D4 and D5.
   ``main()`` pulses A2 every 100 ms, so a single edge raises the EGU2 and
   EGU3 events in the same clock cycle. The spec suggests one TIMER driving
   both lines through PPI; one edge into both inputs is at least as
   simultaneous.
#. Run for at least 1000 pulses. Halt the core with a debugger and save
   the ``ram_tracing`` buffer, then decode it with Zephyr's CTF tooling.
#. In the trace, each pulse produces two ``dm_hw_line`` named events; the
   last argument is the line number. Check the order for every pulse. As
   a cross-check, the ``fired`` counter of each entry of ``lines[]`` in
   ``subsys/dockmaster/hw/dm_hw_nrf_egu.c`` must equal the pulse count.
#. With the debugger, set a breakpoint (or a hit counter) on the GPIOTE
   interrupt handler before the run and confirm it is never hit while only
   the class lines toggle. The PHY's done pin must be idle for this check,
   since it uses an ordinary GPIOTE interrupt.

Record: number of pulses, number of pulses where line 0 came first, the
GPIOTE handler hit count, and the decoded trace.

Pass criteria (from the spec, not yet observed): line 0 first for every
pulse; zero GPIOTE interrupts from the class lines.

Stack sizes
***********

Every report ends with the thread analyzer's output. Record the unused
stack of ``dm_sched``, every ``dm_owner<n>`` and ``main`` after each lab, as
the spec asks for in its definition of done.
