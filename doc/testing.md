# Testing

Every rule in spec sections 7, 8, 10, 11 and 12 has a named ztest. The
suites run on two targets:

- `native_sim` (and `native_sim/native/64`): the host build, fast and
  deterministic.
- `mps2/an385`: a Cortex-M3 under QEMU. Threads are preempted by real timer
  interrupts here, which the host build never does, so this is where races
  show up. It also proves the code uses no M4-only instructions.

The unit suites drive the scheduler synchronously (`dm_sched_round(now)`)
with a fake clock, so token balances and transitions are asserted exactly.
Owner threads still run for real; the fake link completes either inside
`tx` or from a `k_timer`, i.e. from interrupt context like a radio.

## Suites

| Suite | Tests | Covers |
| --- | --- | --- |
| `unit/queue` | 9 | invalid arguments; O(1) submit (cycle bound on M3); quota per class without crossing classes; shed order C4, C3, C2, C1, never C0; coalesce in place; age and deadline drops with C0 exempt; DUP refcount; ISR path returns `-EAGAIN` instead of shedding or spinning; zero-copy `dm_alloc` / `dm_submit_block` |
| `unit/sched` | 14 | strict priority with all buckets wet; dry C3 falls through to C4; C0 overdraws then waits for refill (exact balances); measured airtime charged instead of the estimate; refill across tick wraparound with remainder carry; half-duplex rx-off charge; DUP on two links; DUP pending until a second link appears; DUP window expiry; tx never overlaps on a link; watchdog cancel and fault; C0 retried after failure; MAC priority mapping; sw-EDCA backoff and give-up |
| `unit/linkmgr` | 9 | Q3 EWMA; score monotonic in loss, RTT, throughput, cost and state; switch rule margin and dwell; flap decay; DOWN link excluded until a probe passes; metered links excluded for EXCLUSIVE classes; probe failures degrade then take a link down; switch after margin and dwell with make-before-break; failed make-before-break probe cancels |
| `unit/policy` | 7 (x2) | runs twice on native_sim: RAM store, then Settings on NVS on the flash simulator (asserts the records are really in settings); default validates; reject bad magic, CRC, version, unknown link, empty C0 mask, quota overflow, rate, thresholds, zero C0 rate, with nothing applied; revert swaps slots; persistence across reboot; auto-revert after two reboots in the window; confirmed policy never reverted; atomic apply under a concurrent producer |
| `unit/pressure` | 6 | transitions and hysteresis; boundary class of each hint; C4 refused in TIGHT, C3 in SHED, proactive shed; exactly one hint per transition; C1 coalesce forced in SHED; CRITICAL sticky until reset; property test: 10,000 random submits with one link flapping, C0 never lost |
| `unit/prio` | 6 | `dm_irq_prio` for 2, 3, 4, 8 bits with and without zero-latency IRQs, never 0, macro equals function; owner priority defaults; every init assert; registration rejects bad priorities; MAC slot mapping; EDCA backoff bounds |
| `integration/two_links` | 1 | full stack, real threads, 60 s at 5x nominal load, radio flapping every 7 s, metered link 10 % lossy (see below) |

## Integration run

Two fake links: a 250 kbit/s radio (tier MAC) that goes down and up every
7 s, and a 50 kbit/s metered cellular link (tier SW, sw-EDCA on) that loses
10 % of frames. Producers run at five times a nominal load of C0 1/s, C1
10/s over 8 keys, C2 20/s with a 300 ms deadline, C3 2/s and C4 10/s.

Asserted at the end:

- every C0 message delivered, `c0_lost == 0`;
- for every C1 key, the newest submitted value is the last one delivered;
- zero C4 bytes on the metered link;
- no slab leak, nothing queued;
- `submitted == sent + failed + dropped + coalesced` for every class.

One run on `mps2/an385` (Cortex-M3):

| Class | Submitted | Refused | Sent | Dropped (quota / age / shed) | Coalesced |
| --- | --- | --- | --- | --- | --- |
| C0 | 300 | 0 | 300 | 0 / 0 / 0 | 0 |
| C1 | 3000 | 0 | 923 | 0 / 0 / 0 | 2077 |
| C2 | 6000 | 0 | 1908 | 1766 / 0 / 2302 | 0 |
| C3 | 288 | 312 | 192 | 0 / 0 / 83 | 0 |
| C4 | 544 | 2456 | 544 | 0 / 0 / 0 | 0 |

The overload lands where the class table says it should: bulk (C4) is
refused at the door, media (C2) is shed and aged out, state (C1) is
coalesced, control (C0) is untouched. The test also shows two things about
policy sizing, now written into the test policy:

- C0 may overdraw once but is still paced by its bucket. At 5 messages per
  second over a link that needs 22.6 ms per frame, C0 needs 113 ms of air
  per second; the default 50 ms/s would refuse the excess (correctly, and
  visibly through `c0_lost`).
- C1 must be able to flush every key within its `max_age`.

In total: 52 test cases on `mps2/an385` and 59 on `native_sim` (the policy
suite runs a second time over Settings and NVS), plus the native sample with
its console checks on both.

## Hardware targets (build only)

No board was available, so these are compile and link checks, run in CI
with zero warnings allowed. Sizes include the kernel, logging and the
sample.

| Build | Board | Flash | RAM |
| --- | --- | --- | --- |
| `samples/dk_spi_phy_edca` (tier HW, GPIO probes) | nrf52840dk/nrf52840 | 84.8 KB | 35.5 KB |
| `samples/ncp_uart_link` (UART co-processor + fake radio) | nucleo_l476rg | 66.9 KB | 34.8 KB |
| `tests/build/drivers` (all three drivers, PPI tier 0) | nrf52840dk/nrf52840 | 114.2 KB | 46.5 KB |
| `tests/build/drivers` (SPI PHY, NCP, DPPI tier 0) | nrf5340dk/nrf5340/cpuapp | 79.2 KB | 33.4 KB |

## Running

```sh
# all suites on the host and on the Cortex-M3 model
west twister -T dockmaster/tests -p native_sim -p mps2/an385

# one suite, verbose
west twister -T dockmaster/tests/unit/sched -p native_sim --inline-logs -v
```

On an arm64 Linux host use `native_sim/native/64`; the 32-bit `native_sim`
needs an x86 host with multilib.
