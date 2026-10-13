# Decisions and deviations from the spec

The spec ([spec.md](spec.md)) was implemented as written wherever it was
consistent. Building and testing it surfaced gaps and a few contradictions;
each is resolved below with the reason. Larger ones have an ADR in
[adr/](adr/index.md).

## Contradictions resolved

| # | Spec says | Implementation | Why |
| --- | --- | --- | --- |
| 1 | Default quotas 1, 2, 8, 16, 32 KiB, and "sum of quotas must fit the slab" | Quotas are shares of the slab's payload capacity: C0 10 %, C1 10 %, C2 20 %, C3 25 %, C4 35 % | The table adds up to 59 KiB against a default 8 KiB slab, so the shipped default would fail its own validation. The ordering (C4 largest) is kept. |
| 2 | Section 8 code: C0 is never gated by its bucket. Prose: "C0's own refill must recover before C0 sends again" | C0 may send whenever its balance is not negative, so it can overdraw by one message, then waits for refill | The code alone lets a C0 flood starve every other class; the prose is the stated intent ("without starving everything else forever"). |
| 3 | `k_msgq` for class queues; coalesce "replaces the older block in place" | Class queues are fixed rings of the same 8-byte refs | `k_msgq` has no way to update an entry in place. See [ADR 0004](adr/0004-class-queue-rings.md). |
| 4 | Flap penalty per link, set on the link switched to | Per class and per link, applied only to candidate links | The literal rule lowers the new link's score for every class: one class switching holds all others back, and the class that just moved finds switching back easier (anti-damping). Found by the `linkmgr` suite. |
| 5 | `dm_stats` "under 200 bytes" | `dm_stats_encode()` packs a snapshot into 16 + 80 + 16 per link bytes (160 B for four links) | The struct as specified is about 650 bytes. |

## Gaps filled

- **Airtime charging.** The estimate is reserved at dispatch so a link
  cannot be over-committed while a message is in flight; on completion the
  estimate is credited back and the measured airtime debited. The net charge
  is the measured airtime, as specified.
- **`DM_LINK_MASK_ALL` (0xFF)** means "every registered link, including ones
  registered later". The default policy uses it, so it validates before any
  driver has registered.
- **Extra validation:** `10 <= threshold[0] < threshold[1] <= 100` (the
  hysteresis is 10 points), and C0 needs a non-zero rate on every link it may
  use (a zero rate would block control forever after one overdraw).
- **C1 is state, so the newest value of a key must arrive.** A C1 message
  that failed on every link is re-queued unless a newer message with its key
  exists, queued or in flight. `max_age` still bounds it, so this is not
  general retransmission (still a non-goal). Found by the integration test
  on a 10 % lossy link.
- **C0 is never dropped by age**, whatever its policy says; failed C0 goes to
  a retry ring in front of the C0 queue.
- **Accounting.** Two counters were added per class: `refused` (never
  accepted) and `failed` (failed on every link). With them every accepted
  message is accounted for:
  `submitted = sent + failed + dropped_quota + dropped_age + dropped_shed + coalesced`
  once queues are empty. The integration test asserts this identity.
- **Producer hint.** Called once per transition with the class whose
  boundary was crossed: C2 for TIGHT, C3 for SHED, C0 for CRITICAL.
- **Messages larger than a link's MTU** are sent on that link only with
  `DM_F_RESUMABLE` (the driver chunks).
- **Fault window** for `CONFIG_DM_FAULT_N`: consecutive watchdog faults
  without a successful completion in between.
- **Owner thread above the scheduler** is allowed for `DM_TIER_MAC` links only,
  as section 9 says; there is no field for a driver to "prove" a 1 us ISR, so
  that part is review policy.

## Priorities

`dm_irq_prio(level)` returns the priority argument for `IRQ_CONNECT` in
Zephyr's numbering, where 0 is the most urgent regular level. Logical levels
are sixteenths of the NVIC range, which reproduces the spec's table (1, 2, 3, 5
on 3-bit parts; 1, 3, 6, 10 on 4-bit parts), never returns 0 and clamps to
`IRQ_PRIO_LOWEST`, which shrinks when `CONFIG_ZERO_LATENCY_IRQS` reserves a
level. `DM_IRQ_PRIO_CONST()` is the same formula as a constant expression for
`IRQ_DIRECT_CONNECT`.

## Kconfig beyond the spec's list

| Symbol | Default | Purpose |
| --- | --- | --- |
| `DM_LINK_SLOTS` | 4 | Owner stacks are static; this is how many are reserved |
| `DM_DUP_WINDOW_MS` | 2000 | How long a duplicate copy waits for a second link when the class has no age limit |
| `DM_W_TPUT` .. `DM_W_STATE` | 250, 150, 250, 100, 100, 150 | Score weights (sum 1000), which the spec mentions but does not list |
| `DM_TEST_HOOKS` | n | Manual scheduler rounds, fake clock and full reset for the ztest suites |

`DM_MAX_MSG` is a Kconfig value with a build assertion that it fits in a
block, rather than a derived constant.

## Timing measurements

`native_sim` runs code in zero simulated time: its cycle counter does not
move while code executes. Timing assertions therefore run on `mps2/an385`
(Cortex-M3 under QEMU with `-icount shift=7`, about 7.8 million instructions
per virtual second), where they count executed instructions.

| What | Measured | Spec target |
| --- | --- | --- |
| `dm_submit()` cost vs queue depth | about 2,600 SysTick cycles at depth 0 and at depth 15: constant | O(1): met |
| `dm_submit()` absolute cost | about 800 instructions with `CONFIG_ASSERT=y` | "< 2 us on host": not meaningful under emulation, to be measured on hardware |
| Worst scheduler round, two links, 60 s at 5x load | 671 us of QEMU virtual time with asserts off, including timer ISRs that preempt the round | "< 50 us": not yet shown; needs the hardware lab |

Three costs were removed after profiling the round on the M3 model (no
64-bit divides in the bucket refill, the score and the eviction log rate
limiter; scores recomputed only when their inputs change). The remaining
worst case is dominated by rounds that shed or age out many messages at
once.

## The spec's open questions

| Question | Answer in this implementation |
| --- | --- |
| C2 above C1 on the medium while C1 is above C2 in the queue? | Kept as specified: with four MAC slots C2 maps to slot 1 and C1 to slot 2. Stale state is coalesced anyway. |
| 16-bit `rate_ms_per_s` and `burst_ms`? | Kept. Internally buckets are 32-bit microseconds. |
| ISR `-EAGAIN` or a lock-free SPSC ring per class? | `-EAGAIN`. The ISR path is one append under one short `irq_lock`; it only returns `-EAGAIN` when the request needs coalesce or shed, or a thread holds the consumer flag. Revisit if hardware lab 1 shows contention. |
| Reference CBOR encoder for the policy? | Not written. `dm_policy_seal()` and the packed 214-byte layout are what an encoder would target. |
| Name collision? | Checked on 2026-10-13: no Zephyr, NCS or embedded project is called Dockmaster. PyPI `dockmaster` is an unrelated Docker management tool, crates.io has nothing, and the GitHub results are unrelated Docker utilities. |

## Not done yet

- Hardware labs 1 to 4 (ISR latency, sw-EDCA collisions, tier-0 ordering,
  airtime accuracy). The procedures and scripts are in
  `samples/dk_spi_phy_edca`; no numbers are claimed.
- Sphinx output is built in CI but not published.
- A reference CBOR encoder for the policy (an open question in the spec).
