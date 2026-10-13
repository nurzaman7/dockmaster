# Dockmaster: Firmware QoS for Multi-Radio Cortex-M Devices on Zephyr

Oct 13, 2026 · @Nur

> This is the design specification the implementation was built from, kept as written.
> Where the implementation had to resolve a gap or a contradiction, the decision is
> recorded in [deviations.md](deviations.md) and, for the larger ones, in [adr/](adr/index.md).

## 1. Scope and goals

Dockmaster is a Zephyr module that gives a Cortex-M device with several radios one traffic-class table, one airtime scheduler, and one link selector, enforced from thread priorities down to NVIC priorities. It is radio-agnostic: a radio is anything that implements the `dm_link` interface, whether its MAC runs on the Cortex-M, in an external PHY, or in a co-processor.

**Goals**

- Every outbound message carries a class C0–C4; class decides queue, budget, link eligibility and drop order.
- Producers never block and never allocate: `dm_submit()` is O(1), ISR-safe, and returns a result code instead of waiting.
- Budgets are in milliseconds of airtime, not bytes, so duty-cycle and regulatory limits are scheduler properties.
- Each link has exactly one owner thread; no radio is touched from two contexts.
- Enforcement tiers: software queues always; MAC-native priority when the link reports it; per-class NVIC lines when the SoC can route events to separate IRQs.
- Policy is data: a versioned, CRC-protected table applied at runtime and revertible.
- Host-testable: the scheduler and queues build and run on `native_sim` with fake links.

**Non-goals in v1**: IP routing, TLS, OTA transport, per-packet FEC, link aggregation, anything above the message boundary.

**Target**: Zephyr 3.7 LTS or later, Cortex-M3/M4/M33/M7, 3 or more NVIC priority bits, 32 KB or more RAM available to the module.

## 2. Model and terms

Five classes, one scheduler per link, one owner thread per link. Everything else is derived from this table.

| Term | Definition | Fixed by |
| --- | --- | --- |
| Class `C0`–`C4` | Priority band a message belongs to. C0 = control, C1 = state, C2 = media, C3 = events, C4 = bulk. Lower number wins. | Policy artifact |
| Message | One unit submitted by a producer: `cls`, `len`, `deadline_ms`, flags, pointer into a slab. Never fragmented by Dockmaster. | Producer |
| Link | One radio or network path registered with `dm_link_register()`. Owns an owner thread, a per-link scheduler instance, a capability record. | Link driver |
| Owner thread | The only Zephyr thread that calls the link's `tx`, `cancel`, `probe`. Priority and stack size come from the link record. | Dockmaster |
| Airtime | Milliseconds the link is busy transmitting one message, computed by the link's `cost_of(len)`. The unit all budgets use. | Link driver |
| Token bucket | Per class, per link: `rate_ms_per_s`, `burst_ms`. Dry bucket = class skipped this round. | Policy artifact |
| Affinity | Bitmask of links a class may use, plus `dup` flag (send on two links) and `exclusive` flag (never on cellular-cost links). | Policy artifact |
| Tier | How strictly a link separates classes: `DM_TIER_SW` (Dockmaster queues only), `DM_TIER_MAC` (link exposes native priority slots), `DM_TIER_HW` (SoC routes per-class completion to separate NVIC lines). | Link capability record |
| Shed order | C4 first, then C3, then C2 quality, then C1 coalesce. C0 is never shed. | Fixed in code |

A message's lifetime: `dm_submit` → class queue → scheduler selects per link → owner thread calls `tx` → completion callback frees the slab → stats updated. At no point does a producer thread wait on a radio.

## 3. Target platform assumptions

The module assumes a single Cortex-M core running Zephyr with the NVIC as the only interrupt controller. The agent must not use Zephyr features absent from this list.

**Cortex-M**

- NVIC priority width is read at build time from `CONFIG_NUM_IRQ_PRIO_BITS` (3 on nRF52, 4 on STM32L4, up to 8). Lower number = higher urgency. All priority arithmetic goes through one helper, `dm_irq_prio(level)`, which clamps to the available range and never returns 0 (reserved for zero-latency IRQs and vendor radio stacks).
- `BASEPRI` is used, never `PRIMASK`: critical sections in Dockmaster use `irq_lock()`/`irq_unlock()` only around the single-word queue head/tail updates, and must be under 1 µs.
- All DMA-visible buffers live in RAM, 4-byte aligned; on M7 targets with D-cache they are placed in a non-cacheable region via `K_MEM_SLAB_DEFINE` in a linker section the board defines (`CONFIG_DM_NOCACHE_SECTION`).
- `DSB` after clearing any peripheral event register before returning from an ISR (Zephyr drivers already do this; Dockmaster's own ISRs must too).

**Zephyr**

- Threads: `K_THREAD_DEFINE` or `k_thread_create` with static stacks. No dynamic thread creation at runtime.
- Queues: `k_msgq` for the class queues (fixed element size = `struct dm_msg_ref`, 8 bytes), `k_mem_slab` for payload buffers. No `k_malloc` anywhere in the module.
- Interrupts: `IRQ_CONNECT` for any ISR the module owns; `irq_enable`/`irq_disable`; `IRQ_DIRECT_CONNECT` only for `DM_TIER_HW` completion lines.
- Work: the module never submits to the system workqueue. Deferred work runs on the owner thread or on the one scheduler thread.
- Timing: `k_uptime_ticks()` for bucket refill; `k_cycle_get_32()` for airtime measurement; `k_timer` for dwell and probe periods.
- Policy storage: `settings` subsystem with NVS backend, key `dm/policy`; two slots, active and previous.
- Build: a Zephyr module (`zephyr/module.yml`), Kconfig under `CONFIG_DOCKMASTER`, devicetree binding `dockmaster,link` for per-link thread priority, stack size, and cost hints.

**Memory budget, default config**: 5 classes × 16 refs × 8 B = 640 B of queue; slab 32 × 256 B = 8 KB; per link 1 thread stack (default 2 KB) + 256 B state; scheduler thread 1.5 KB. Four links ≈ 20 KB total.

## 4. Architecture

The seven layers and the data flow are drawn in [architecture.md](architecture.md).

Data flows down; the only upward path is the scheduler's queue-depth signal, which the policy reads to tighten classification. Producers, the scheduler, the link manager and each owner thread are separate Zephyr threads; nothing runs on the system workqueue.

## 5. Public API

Two headers: `include/dockmaster/dm.h` for producers and policy, `include/dockmaster/dm_link.h` for link drivers. Everything else is private under `subsys/dockmaster/`.

**`dm.h`: producer and policy side**

```c
typedef enum { DM_C0 = 0, DM_C1, DM_C2, DM_C3, DM_C4, DM_CLASS_COUNT } dm_class_t;

#define DM_F_COALESCE   BIT(0)  /* replace older message with same key (C1) */
#define DM_F_DROP_LATE  BIT(1)  /* discard if deadline passes before tx (C2) */
#define DM_F_RESUMABLE  BIT(2)  /* driver may chunk and resume (C3, C4) */
#define DM_F_DUP        BIT(3)  /* send on two links if affinity allows (C0) */

struct dm_submit_opts {
    uint32_t deadline_ms;   /* 0 = none; relative to k_uptime_get() */
    uint32_t coalesce_key;  /* used only with DM_F_COALESCE */
    uint8_t  link_hint;     /* DM_LINK_ANY or a registered link index */
};

/* Copies len bytes into a slab and enqueues. Safe from ISR. Never blocks.
 * Returns 0, -ENOMEM (slab empty, caller may retry later), -ENOSPC (class quota
 * full and nothing lower to evict), -EINVAL (len > DM_MAX_MSG), -EAGAIN (ISR
 * context and queue lock contended; caller should defer). */
int dm_submit(dm_class_t cls, const void *buf, size_t len,
              uint32_t flags, const struct dm_submit_opts *opts);

/* Zero-copy variant: caller owns a slab block obtained from dm_alloc(). */
void *dm_alloc(size_t len, k_timeout_t timeout);
int   dm_submit_block(dm_class_t cls, void *block, size_t len,
                      uint32_t flags, const struct dm_submit_opts *opts);

int  dm_policy_apply(const struct dm_policy *p);     /* validates, CRC, atomically swaps */
int  dm_policy_revert(void);                          /* back to previous slot */
const struct dm_policy *dm_policy_current(void);

void dm_stats_get(struct dm_stats *out);              /* copy-out, lock-free snapshot */
void dm_stats_reset(void);

int  dm_init(void);                                   /* SYS_INIT at APPLICATION level */
```

**`dm_link.h`: driver side**

```c
#define DM_TIER_SW   0
#define DM_TIER_MAC  1
#define DM_TIER_HW   2

struct dm_link_caps {
    uint16_t mtu;             /* max bytes per tx call */
    uint8_t  tier;            /* DM_TIER_* */
    uint8_t  mac_prio_levels; /* 0 if none; else count of native priority slots */
    bool     half_duplex;     /* tx blocks rx */
    bool     metered;         /* bytes cost money; affinity "exclusive" applies */
    uint32_t probe_period_ms; /* 0 = never probe */
    uint8_t  owner_prio;      /* Zephyr thread priority for the owner thread */
    size_t   owner_stack;     /* bytes; 0 = CONFIG_DM_DEFAULT_OWNER_STACK */
};

struct dm_link_ops {
    /* Called only from the owner thread. Must return quickly; completion
     * arrives via dm_link_tx_done(). May be called with mac_prio when
     * caps.tier >= DM_TIER_MAC. */
    int      (*tx)(const struct device *dev, const uint8_t *buf, size_t len,
                   uint8_t mac_prio, uint32_t *handle);
    int      (*cancel)(const struct device *dev, uint32_t handle);
    /* Airtime in microseconds for len bytes at current link settings. Pure. */
    uint32_t (*cost_of_us)(const struct device *dev, size_t len);
    /* Optional end-to-end probe; result via dm_link_probe_done(). */
    int      (*probe)(const struct device *dev);
    /* Optional: true while medium is busy (CAD, CCA, module busy pin). */
    bool     (*is_busy)(const struct device *dev);
    /* Optional, DM_TIER_HW only: install per-class completion IRQ at prio. */
    int      (*bind_class_irq)(const struct device *dev, dm_class_t cls,
                               unsigned int irqn, unsigned int prio);
};

int dm_link_register(const struct device *dev, const struct dm_link_ops *ops,
                     const struct dm_link_caps *caps, uint8_t *out_index);

/* Driver → Dockmaster, callable from ISR. */
void dm_link_tx_done(uint8_t link, uint32_t handle, int status, uint32_t airtime_us);
void dm_link_probe_done(uint8_t link, uint32_t rtt_us, uint8_t loss_pct);
void dm_link_state(uint8_t link, enum dm_link_state s);  /* UP, DEGRADED, DOWN */
```

**Contracts the agent must enforce with asserts and tests**

- `dm_submit` from an ISR takes at most one `irq_lock` section; if the lock is held it returns `-EAGAIN` rather than spinning.
- `ops->tx` is never called while a previous `tx` on the same link is outstanding unless `caps.mac_prio_levels > 1`.
- `dm_link_tx_done` must be called exactly once per accepted `tx`; a missing completion is detected by a per-link watchdog of `4 × cost_of_us(len)` and counted as a link fault.
- `cost_of_us` is pure and must not touch hardware.

## 6. Class table and policy artifact

The policy is one packed, versioned C struct; it is the only thing that changes behaviour at runtime. It is applied atomically, stored in two NVS slots, and auto-reverted if the device reboots twice within `revert_window_s` after applying it.

```c
#define DM_POLICY_MAGIC   0x444D5031u  /* "DMP1" */
#define DM_MAX_LINKS      8

struct dm_bucket {
    uint16_t rate_ms_per_s;   /* sustained airtime budget, ms of air per second */
    uint16_t burst_ms;        /* bucket depth */
};

struct dm_class_policy {
    uint32_t quota_bytes;     /* max bytes this class may hold in slabs */
    uint16_t max_age_ms;      /* 0 = no age limit; else drop when exceeded */
    uint8_t  link_mask;       /* which link indices may carry this class */
    uint8_t  flags;           /* DM_P_DUP, DM_P_EXCLUSIVE_NO_METERED, DM_P_COALESCE */
    struct dm_bucket bucket[DM_MAX_LINKS];
};

struct dm_policy {
    uint32_t magic;
    uint16_t version;          /* monotonically increasing; applied only if > current */
    uint16_t revert_window_s;
    uint8_t  depth_tighten_pct[2]; /* queue depth thresholds, e.g. {60, 85} */
    struct dm_class_policy cls[DM_CLASS_COUNT];
    uint32_t crc32;            /* over everything above */
};
```

**Default policy shipped in `dm_policy_default.c`** (the agent writes this file; values are the starting point, not sacred):

| Class | quota\_bytes | max\_age\_ms | link\_mask | flags | bucket on a slow link (ms/s, burst) |
| --- | --- | --- | --- | --- | --- |
| C0 | 1024 | 0 | all | DUP | 50, 200 |
| C1 | 2048 | 5000 | all | COALESCE | 30, 100 |
| C2 | 8192 | 300 | non-metered preferred | none | 200, 400 |
| C3 | 16384 | 0 | all | none | 100, 1000 |
| C4 | 32768 | 0 | non-metered only | EXCLUSIVE\_NO\_METERED | 500, 5000 |

**Validation on apply** (reject with `-EINVAL`, never partially apply): magic and CRC match; `version` greater than current; every `link_mask` bit refers to a registered link; C0 `link_mask` non-zero; sum of `quota_bytes` ≤ slab capacity; every `rate_ms_per_s` ≤ 1000.

## 7. Queues and memory

One slab pool holds every payload; five `k_msgq`s hold 8-byte references. A producer's cost is one slab alloc, one memcpy, one msgq put, all with `K_NO_WAIT`.

```c
struct dm_msg_ref {            /* 8 bytes, what sits in the k_msgq */
    uint16_t slab_idx;
    uint16_t len;
    uint32_t enq_tick;          /* k_uptime_ticks() at submit, low 32 bits */
};

struct dm_msg_hdr {            /* first 16 bytes of each slab block */
    uint32_t deadline_tick;
    uint32_t coalesce_key;
    uint8_t  cls, flags, link_hint, dup_pending;
    uint16_t refcnt;            /* 2 when DUP, else 1 */
    uint16_t handle;
};

K_MEM_SLAB_DEFINE_STATIC(dm_slab, CONFIG_DM_SLAB_BLOCK, CONFIG_DM_SLAB_COUNT, 4);
K_MSGQ_DEFINE(dm_q0, sizeof(struct dm_msg_ref), CONFIG_DM_Q_DEPTH_C0, 4);  /* …q1..q4 */
```

**Rules, each with a unit test**

1. **Recording never blocks.** `dm_submit` uses `k_mem_slab_alloc(K_NO_WAIT)` and `k_msgq_put(K_NO_WAIT)`. On slab exhaustion it runs the shed routine once (below) and retries once; still full → `-ENOMEM`.
2. **Byte quota per class.** `cls_bytes[cls]` is tracked atomically; a submit that would exceed `quota_bytes` triggers shed within that class first (oldest C4 → oldest C3 → oldest C2 → coalesce C1), never across into a higher class.
3. **Shed order is structural.** `dm_shed(need_bytes)` walks C4, C3, C2, C1 in that order, evicting oldest-first, and never touches C0. Each eviction increments `stats.dropped[cls]` and logs at `LOG_WRN` with a 1 s rate limit.
4. **Coalesce.** With `DM_F_COALESCE`, a submit with a `coalesce_key` already present in that class replaces the older block in place (same slab index, updated len and tick) rather than enqueueing a second reference. Lookup is a linear scan of that class's queue (depth ≤ 16, so bounded).
5. **Age.** The scheduler drops any head-of-queue message whose `enq_tick + max_age_ms` has passed, before considering it for a link.
6. **DUP.** A `DM_F_DUP` message holds `refcnt = 2`; the slab block is freed on the second `dm_link_tx_done`. If only one link is eligible at submit time, `refcnt = 1` and `dup_pending = 1`; the scheduler sends the copy when a second link becomes eligible within `max_age_ms`.
7. **ISR path.** From ISR, `dm_submit` skips coalesce and shed (both may be O(depth)) and returns `-EAGAIN` instead; the caller defers to a thread. Documented in the header.

**Cache and DMA.** Slab memory is `__aligned(32)` and, when `CONFIG_DM_NOCACHE_SECTION` is set, placed in the board's non-cacheable region so EasyDMA/DMA-fed drivers can transmit straight from the block. Drivers that need their own buffer copy once inside `tx`; Dockmaster never copies after submit.

## 8. Scheduler

One scheduler thread serves all links. Each round it picks, for every idle link, the highest class that has a message, a wet bucket, and affinity for that link; it then hands one message to that link's owner thread via a per-link `k_msgq` of depth 2. Strict priority decides order; buckets decide whether a class may use the link at all this round.

```c
struct dm_bucket_state { int32_t tokens_us; uint32_t last_refill_tick; };
static struct dm_bucket_state bucket[DM_CLASS_COUNT][DM_MAX_LINKS];

static void refill(dm_class_t c, uint8_t l, uint32_t now_tick)
{
    const struct dm_bucket *p = &dm_policy_current()->cls[c].bucket[l];
    uint32_t dt_us = k_ticks_to_us_floor32(now_tick - bucket[c][l].last_refill_tick);
    int64_t add = (int64_t)dt_us * p->rate_ms_per_s / 1000;      /* ms-per-s × s = ms; ×1000 = us */
    bucket[c][l].tokens_us = MIN(bucket[c][l].tokens_us + add, (int32_t)p->burst_ms * 1000);
    bucket[c][l].last_refill_tick = now_tick;
}

/* Returns the class to serve on link l, or DM_CLASS_COUNT if none. */
static dm_class_t pick(uint8_t l, uint32_t now_tick)
{
    for (dm_class_t c = DM_C0; c < DM_CLASS_COUNT; c++) {
        if (!(dm_policy_current()->cls[c].link_mask & BIT(l))) continue;
        if (k_msgq_num_used_get(&dm_q[c]) == 0) continue;
        refill(c, l, now_tick);
        struct dm_msg_ref head; k_msgq_peek(&dm_q[c], &head);
        uint32_t cost = links[l].ops->cost_of_us(links[l].dev, head.len);
        if (bucket[c][l].tokens_us < (int32_t)cost && c != DM_C0) continue; /* dry: fall through */
        return c;
    }
    return DM_CLASS_COUNT;
}
```

**Design decisions the agent must keep**

- **C0 may overdraw.** A dry C0 bucket still sends; tokens go negative and C0's own refill must recover before C0 sends again. This is the "never drop control" guarantee without starving everything else forever.
- **Airtime is charged on completion, not on dispatch.** `dm_link_tx_done` carries the measured `airtime_us`; the scheduler debits the measured value and credits back any over-estimate from `cost_of_us`. Estimates drift; measurements do not.
- **Half-duplex links** (`caps.half_duplex`) are also charged for receive windows they close: the driver reports `airtime_us` including the RX-off time, so a chatty C3 cannot silently blind the link's RX.
- **Dwell between classes on one link** is zero; the only pacing is the bucket. If a link needs guard time, that is the driver's `cost_of_us`.
- **One scheduler thread**, priority `CONFIG_DM_SCHED_PRIO` (default 4, cooperative not allowed), woken by: a submit into an empty class, a `tx_done`, a bucket refill timer (period = smallest `burst_ms / 4` across the table, min 10 ms), a link state change. Wake-ups coalesce through one `k_poll` with a signal.
- **Round cost is bounded**: 5 classes × N links × one `cost_of_us` call. No allocation, no searching beyond `k_msgq_peek`.

**Software EDCA for `DM_TIER_SW` links that expose `is_busy`.** Before the owner thread calls `tx`, it waits `aifs_us[cls] + random(0, cw_us[cls])`, re-checking `is_busy` after the wait; busy → double `cw` up to `cw_max[cls]` and retry, up to `retries[cls]`. Defaults: C0 `aifs` 2 ms, `cw` 0–8 ms; C4 `aifs` 50 ms, `cw` 0–400 ms. These live in Kconfig, not the policy, because they depend on the PHY's symbol time. The wait uses `k_sleep` on the owner thread, never a busy loop.

## 9. Link abstraction and owner threads

A link is a Zephyr `struct device` plus `dm_link_ops` plus `dm_link_caps`. Dockmaster creates one owner thread per registered link at `dm_init()`; the driver never creates threads and never calls its own `tx` from any other context.

**Owner thread loop** (`subsys/dockmaster/dm_owner.c`):

```c
static void owner_thread(void *p1, void *p2, void *p3)
{
    struct dm_link *l = p1;
    for (;;) {
        struct dm_dispatch d;
        k_msgq_get(&l->dispatch_q, &d, K_FOREVER);         /* from scheduler */
        if (l->caps.tier == DM_TIER_SW && l->ops->is_busy) {
            sw_edca_wait(l, d.cls);                        /* section 8 */
        }
        uint32_t t0 = k_cycle_get_32(), handle;
        int rc = l->ops->tx(l->dev, d.buf, d.len, mac_prio_for(l, d.cls), &handle);
        if (rc) { dm_link_tx_done(l->index, 0, rc, 0); continue; }
        /* Block only on this link's own completion, with a watchdog. */
        rc = k_sem_take(&l->done_sem, K_USEC(4 * l->ops->cost_of_us(l->dev, d.len)));
        if (rc == -EAGAIN) { l->ops->cancel(l->dev, handle); dm_link_fault(l, DM_FAULT_NO_COMPLETION); }
    }
}
```

**Mapping a class to a native MAC priority** (`DM_TIER_MAC`): `mac_prio_for()` scales C0–C4 onto `caps.mac_prio_levels`. For 4 levels: C0→0, C1→2, C2→1, C3→2, C4→3 (C2 media ranks above C1 state on the medium, matching Wi-Fi VI above BE). For 2 levels: C0–C2→0, C3–C4→1. For 1 level or `DM_TIER_SW`: always 0 and the argument is ignored.

**Thread priorities** come from the link's devicetree node (`dockmaster,owner-priority`), defaulting to `CONFIG_DM_SCHED_PRIO + 1`. The invariant checked at init: every owner thread is strictly lower priority (numerically higher) than the scheduler thread, and no owner thread is cooperative. A link whose MAC runs on the core (its own ISR at NVIC priority ≤ 1) is allowed an owner thread above the scheduler only if `caps.tier == DM_TIER_MAC`; the agent enforces this with `__ASSERT` at registration.

**Three reference drivers ship in `drivers/dockmaster_link/`**, each under 300 lines, chosen to cover the three offload patterns without naming a product:

| Driver | Pattern | What it exercises |
| --- | --- | --- |
| `dm_link_onchip_ieee802154.c` | MAC on the core | wraps Zephyr's `ieee802154` radio API; `DM_TIER_MAC` with 2 levels via the API's `tx_mode`; completion from the radio ISR |
| `dm_link_spi_phy.c` | external PHY over SPI | generic command + FIFO transceiver using `spi_dt_spec`; one GPIO IRQ for done; `is_busy` via a busy pin; `DM_TIER_SW`; candidate for `DM_TIER_HW` via section 11 |
| `dm_link_ncp_uart.c` | co-processor over UART or I2C | line-oriented request/response; `metered = true`; `probe` sends a round-trip; `DM_TIER_SW` |

A fourth, `dm_link_fake.c`, runs on `native_sim` with configurable latency, loss and busy periods and is what every unit test uses.

## 10. Link manager

The link manager answers one question per class: which eligible link is preferred right now. It runs inside the scheduler thread (no extra thread), re-scores on every `probe_done`, `tx_done` and state change, and switches a class's preferred link only when the new score beats the current one by a margin and the current link has been held for at least the dwell time.

**Score, integer arithmetic only**

```c
/* All terms scaled to 0..1000. Weights sum to 1000 in the default Kconfig. */
score = w_tput  * ewma_tput_norm
      - w_rtt   * ewma_rtt_norm
      - w_loss  * ewma_loss_pct * 10
      - w_flap  * flap_penalty
      - w_cost  * (caps.metered ? 1000 : 0)
      + w_state * (state == DM_LINK_UP ? 1000 : state == DM_LINK_DEGRADED ? 400 : 0);
```

- `ewma_*` use shift-based EWMA (`v += (x - v) >> 3`), updated from measured `airtime_us` and bytes on every `tx_done`, and from `probe_done` when the driver provides one. The modem's own RSSI/RSRP is never an input; only delivered throughput, RTT and loss.
- `flap_penalty` starts at 1000 on each switch and halves every `CONFIG_DM_FLAP_HALFLIFE_S` (default 60 s).
- A link in `DM_LINK_DOWN` scores 0 and is removed from every class's eligible set until it reports `UP` and passes one probe.

**Switch rule, per class**

```c
if (best != cur
    && score[best] > score[cur] + score[cur] * CONFIG_DM_SWITCH_MARGIN_PCT / 100
    && now - held_since[cls] >= dwell_ms(cls)) {
    cur = best; held_since[cls] = now; flap_penalty[best] = 1000;
}
```

`dwell_ms` defaults: C0 0 (control follows the best link immediately), C1 2 s, C2 5 s, C3 30 s, C4 300 s. Make-before-break is the default: the new link is probed before the switch; a failed probe cancels it.

**Affinity and duplication.** The eligible set for a class is `policy.cls[c].link_mask & links_up`. `DM_P_DUP` (C0 by default) sends on the two best eligible links; if only one is up, `dup_pending` is set as in section 7. `DM_P_EXCLUSIVE_NO_METERED` (C4 by default) removes every `caps.metered` link from the set regardless of score; this is the "bulk never on the expensive radio" rule, and it is a policy bit, not an `if` in the driver.

**Steer, duplicate, aggregate.** v1 implements steer (one link per class) and duplicate (C0 on two). Aggregation (striping one class across links) is explicitly out of scope; the interface leaves room for it as a later `DM_P_STRIPE` flag.

## 11. Interrupt and priority plan

The class table is also the thread table and, where the SoC allows it, the NVIC table. This section is normative: the agent implements `dm_prio.c` from it and the init-time asserts below.

**NVIC levels** (computed by `dm_irq_prio(level)` from `CONFIG_NUM_IRQ_PRIO_BITS`; shown here for 3 bits, nRF52-class, and 4 bits, STM32L4-class)

| Logical level | 3-bit | 4-bit | Reserved for |
| --- | --- | --- | --- |
| 0 | 0 | 0 | vendor radio stacks, zero-latency IRQs. Dockmaster never uses it. |
| `DM_IRQ_HW_C0` | 1 | 1 | `DM_TIER_HW` completion line for C0–C2 |
| `DM_IRQ_LINK` | 2 | 3 | any link's native completion ISR (`DM_TIER_SW`, `DM_TIER_MAC`) |
| `DM_IRQ_HW_C4` | 3 | 6 | `DM_TIER_HW` completion line for C3–C4 |
| `DM_IRQ_EDGE` | 5 | 10 | GPIO/edge inputs from external radios |
| lowest | 7 | 15 | SysTick, Zephyr's own |

**Zephyr thread priorities** (preemptible, numeric; lower runs first)

| Thread | Default | Rule |
| --- | --- | --- |
| BT/802.15.4 host RX threads, if present | 0–3 | vendor-owned, above Dockmaster |
| `dm_sched` | 4 | above every owner thread |
| owner thread, link with `caps.tier >= DM_TIER_MAC` | 5 | may be 3 only if the driver proves its ISR is ≤ 1 µs |
| owner thread, `DM_TIER_SW` link | 6 |  |
| owner thread, `caps.metered` link | 7 | bulk-capable links never above others |
| application threads | 8+ |  |
| system workqueue | Zephyr default | Dockmaster never submits to it |

**ISR rules, enforced by review and by `CONFIG_DM_ISR_ASSERTS`**

1. A Dockmaster-owned ISR does exactly three things: read the event, clear it at the source (then `__DSB()`), and either `k_sem_give` the owner's `done_sem` or `k_msgq_put(K_NO_WAIT)` a reference. No `cost_of_us`, no scheduling, no logging above `LOG_DBG`.
2. `dm_link_tx_done` from ISR measures `airtime_us` as `k_cyc_to_us_floor32(k_cycle_get_32() - l->t0_cyc)` and stores it; the scheduler, not the ISR, debits the bucket.
3. Edge-input ISRs (`DM_IRQ_EDGE`) only mark `l->pending_irq = 1` and give a semaphore; the owner thread reads the radio over its bus.
4. No ISR holds `irq_lock` for more than one queue operation.

**Tier 0 on SoCs with an event-routing fabric** (nRF PPI/DPPI + EGU, STM32 with EXTI lines on distinct IRQs, RP2040 PIO IRQs). The agent implements `DM_TIER_HW` as a per-SoC backend under `subsys/dockmaster/hw/`, first for nRF52/nRF53:

```c
/* drivers call this once per class they can separate; Dockmaster picks the
 * NVIC level from the table above and connects the ISR. */
int dm_hw_bind_class_line(uint8_t link, dm_class_t cls,
                          unsigned int egu_idx, unsigned int gpiote_ch);

/* nRF backend: GPIOTE IN[ch] --PPI--> EGU[idx].TRIGGER[0]; EGU[idx] IRQ at
 * DM_IRQ_HW_C0 for cls <= C2, DM_IRQ_HW_C4 for cls >= C3. The ISR gives
 * link->done_sem and records which class completed. */
```

Before each `tx` on a `DM_TIER_HW` link, the owner thread calls `ops->bind_class_irq(dev, cls, …)` so the radio routes that transmission's completion to the class's pin. This is TX-side only; RX arrival on an external PHY stays `DM_TIER_SW` because the class is not known until the bytes are read. A link may therefore report `tier = DM_TIER_HW` for TX and the scheduler treats RX as tier SW automatically.

**Init-time asserts** (`dm_init` fails with `-EINVAL` and logs which one): scheduler priority < every owner priority; no owner thread cooperative; every `DM_TIER_HW` class line has a distinct `IRQn`; no Dockmaster IRQ at level 0; `CONFIG_ZERO_LATENCY_IRQS` offset accounted for in `dm_irq_prio`.

## 12. Backpressure and degradation

Degradation is a state machine driven by one number, total slab occupancy as a percentage, with the thresholds from `policy.depth_tighten_pct`. The scheduler evaluates it after every submit and every `tx_done`; transitions are logged once each.

| State | Enter when occupancy | Effect | Leave when |
| --- | --- | --- | --- |
| NORMAL | < threshold\[0\] | none | n/a |
| TIGHT | ≥ threshold\[0\] (default 60%) | C2 producers told to reduce rate via `dm_hint` callback; C4 submits return `-ENOSPC` immediately | < threshold\[0\] − 10 |
| SHED | ≥ threshold\[1\] (default 85%) | C3 submits also return `-ENOSPC`; scheduler runs `dm_shed()` proactively each round; C1 coalesce forced on | < threshold\[1\] − 10 |
| CRITICAL | slab exhausted and C0 submit failed | `stats.c0_lost++`, `LOG_ERR`, optional `k_oops()` under `CONFIG_DM_C0_LOSS_FATAL` | never silently; requires explicit `dm_stats_reset` or reboot |

**Producer hint.** Applications may register `void (*dm_hint)(dm_class_t cls, enum dm_pressure p)`; it is called from the scheduler thread, once per transition, never from ISR. A media producer uses it to drop bitrate; a logger uses it to stop buffering. The hint is advisory; the quotas above are the enforcement.

**Link-level degradation.** A link that misses `CONFIG_DM_PROBE_FAIL_N` consecutive probes (default 3) or has `CONFIG_DM_FAULT_N` watchdog faults in a window goes `DEGRADED`; its score drops and C0 duplication is forced on if any other link is up. A further failure goes `DOWN`, which triggers the driver's `reset` sequence if it provides one (`probe` → `cancel` all → driver-defined). Recovery back to `UP` requires one passed probe plus a `dwell`.

**What is never done:** drop C0; block a producer; grow a queue; allocate from the heap; recover a link by rebooting the device. Rebooting is the application's decision, informed by `dm_stats`.

## 13. Observability

`dm_stats` is a plain struct of counters, snapshot by `dm_stats_get()` in under 10 µs with one `irq_lock`. It is sized so the application can send it as a C1 message every 10 s at under 200 bytes.

```c
struct dm_class_stats {
    uint32_t submitted, sent, dropped_quota, dropped_age, dropped_shed, coalesced;
    uint32_t oldest_unsent_ms;     /* age of head-of-queue, 0 if empty */
    uint32_t bytes_queued;
    uint32_t airtime_ms_used;      /* measured, since reset */
};
struct dm_link_stats {
    uint32_t tx_ok, tx_fail, faults, probes_ok, probes_fail, switches_to;
    uint32_t ewma_rtt_us, ewma_tput_bps;
    uint16_t ewma_loss_pct_x10, score;
    uint8_t  state, cur_tier;
    uint32_t busy_wait_ms;         /* sw-EDCA time spent waiting, cumulative */
};
struct dm_stats {
    uint16_t policy_version;
    uint8_t  pressure_state;       /* NORMAL, TIGHT, SHED, CRITICAL */
    uint8_t  slab_pct;
    uint32_t c0_lost;              /* must be 0 */
    uint32_t sched_rounds, sched_max_us;
    struct dm_class_stats cls[DM_CLASS_COUNT];
    struct dm_link_stats  link[DM_MAX_LINKS];
};
```

**Shell commands** (`CONFIG_DM_SHELL`): `dm stats`, `dm policy show`, `dm policy revert`, `dm link <n> probe`, `dm link <n> down|up` (test only), `dm submit <cls> <len>` (fills with a pattern).

**Trace points** (`CONFIG_DM_TRACING`, uses Zephyr's `tracing` subsystem so it works with SEGGER SystemView and Percepio): `dm_submit`, `dm_sched_pick`, `dm_owner_tx`, `dm_tx_done`, `dm_switch`, `dm_shed`. Each carries class, link and the message handle, so one trace shows a message's full path and the gap between pick and tx (scheduler latency) and between tx and done (airtime).

**GPIO probe points** (`CONFIG_DM_GPIO_PROBES`, two pins from devicetree): pin A toggles on entry to any Dockmaster ISR, pin B on owner-thread `tx` call. A logic analyser on both gives interrupt-to-tx latency without any instrumentation code in the driver. This is how the hardware labs in section 15 are measured.

## 14. Repository layout, Kconfig, devicetree, build

The repo is a Zephyr module consumable through `west.yml`; nothing in it requires forking Zephyr. License Apache-2.0, matching Zephyr.

```
dockmaster/
├── zephyr/module.yml              # name: dockmaster, cmake-ext, kconfig-ext
├── CMakeLists.txt
├── Kconfig                        # CONFIG_DOCKMASTER and every CONFIG_DM_*
├── include/dockmaster/
│   ├── dm.h                       # section 5, producer and policy API
│   ├── dm_link.h                  # section 5, driver API
│   └── dm_stats.h                 # section 13
├── subsys/dockmaster/
│   ├── dm_core.c                  # init, registration, asserts (section 11)
│   ├── dm_queue.c                 # slabs, class queues, shed, coalesce (section 7)
│   ├── dm_sched.c                 # scheduler thread, buckets (section 8)
│   ├── dm_owner.c                 # owner threads, sw-EDCA (section 9)
│   ├── dm_linkmgr.c               # scoring, switching (section 10)
│   ├── dm_policy.c                # validate, apply, NVS slots, revert (section 6)
│   ├── dm_policy_default.c
│   ├── dm_pressure.c              # section 12
│   ├── dm_stats.c                 # section 13
│   ├── dm_shell.c
│   ├── dm_prio.c                  # dm_irq_prio(), thread prio table
│   └── hw/
│       ├── dm_hw_none.c           # DM_TIER_HW stubs
│       └── dm_hw_nrf_egu.c        # PPI/DPPI + EGU backend
├── drivers/dockmaster_link/
│   ├── Kconfig, CMakeLists.txt
│   ├── dm_link_fake.c             # native_sim
│   ├── dm_link_onchip_ieee802154.c
│   ├── dm_link_spi_phy.c
│   └── dm_link_ncp_uart.c
├── dts/bindings/
│   ├── dockmaster,link.yaml       # owner-priority, owner-stack, metered, tier, probe-period-ms
│   └── dockmaster,hw-nrf-egu.yaml # egu and ppi channel assignments per class
├── samples/
│   ├── two_links_native/          # fake links, prints stats; the demo and the CI smoke test
│   └── dk_spi_phy_edca/           # section 15 hardware lab
├── tests/
│   ├── unit/queue, unit/sched, unit/linkmgr, unit/policy, unit/pressure   # ztest, native_sim
│   └── integration/two_links      # full stack on native_sim with fault injection
└── doc/                           # this spec, rendered; ADRs under doc/adr/
```

**Kconfig, the complete list for v1** (defaults in parentheses): `DOCKMASTER` (n), `DM_SLAB_BLOCK` (256), `DM_SLAB_COUNT` (32), `DM_Q_DEPTH_C0..C4` (8, 8, 16, 16, 16), `DM_MAX_MSG` (= slab block − 16), `DM_SCHED_PRIO` (4), `DM_SCHED_STACK` (1536), `DM_DEFAULT_OWNER_STACK` (2048), `DM_SWITCH_MARGIN_PCT` (20), `DM_FLAP_HALFLIFE_S` (60), `DM_PROBE_FAIL_N` (3), `DM_FAULT_N` (3), `DM_EDCA` (y), `DM_EDCA_AIFS_US_C0..C4`, `DM_EDCA_CW_US_C0..C4`, `DM_EDCA_RETRIES` (4), `DM_TIER_HW_NRF` (n), `DM_NOCACHE_SECTION` (n), `DM_C0_LOSS_FATAL` (n), `DM_ISR_ASSERTS` (y in debug), `DM_SHELL` (n), `DM_TRACING` (n), `DM_GPIO_PROBES` (n), `DM_LOG_LEVEL`.

**Devicetree example** (a link node the agent's sample uses):

```dts
radio_b: dm-link-b {
    compatible = "dockmaster,link";
    owner-priority = <6>;
    owner-stack = <2048>;
    tier = "sw";             /* "sw" | "mac" | "hw" */
    metered;
    probe-period-ms = <30000>;
    backend = <&spi1>;       /* phandle the driver uses */
};
```

**Build targets the CI must pass**: `native_sim` (all tests, both samples), `nrf52840dk/nrf52840` (sample `dk_spi_phy_edca`, `DM_TIER_HW_NRF=y`), `nucleo_l476rg` (sample with the ncp driver, 4-bit NVIC path), `mps2/an385` (Cortex-M3, QEMU, proves no M4-only instructions). `west build -t compliance` clean.

## 15. Testing strategy

Every rule in sections 7, 8, 10 and 12 has a named ztest on `native_sim` using `dm_link_fake`; hardware labs confirm the numbers the host cannot: ISR latency, airtime, and NVIC ordering.

**Unit tests (ztest, `native_sim`, run in CI on every PR)**

| Suite | Tests the agent writes | Pass criterion |
| --- | --- | --- |
| `queue` | submit O(1); quota per class; shed order C4→C3→C2→C1 never C0; coalesce replaces in place; age drop; DUP refcnt; ISR path returns `-EAGAIN` not blocks | assertions plus `k_cycle_get_32` bound on submit < 2 µs on host |
| `sched` | strict priority with all buckets wet; dry C3 falls through to C4; C0 overdraws; measured airtime debits not estimate; refill arithmetic across tick wrap; half-duplex charge | deterministic fake clock, exact token values asserted |
| `linkmgr` | EWMA update; score monotonic in loss; switch only above margin and after dwell; flap penalty decay; DOWN link excluded; metered link excluded for EXCLUSIVE class; make-before-break probe failure cancels | sequence tests with scripted `probe_done` |
| `policy` | reject bad CRC, old version, unknown link bit, empty C0 mask, quota overflow; apply is atomic under concurrent submit; revert restores previous; double-reboot auto-revert | NVS emulated in RAM |
| `pressure` | NORMAL→TIGHT→SHED→CRITICAL transitions and hysteresis; C4 then C3 refused; `dm_hint` called once per transition; `c0_lost` stays 0 under any load with ≥ 1 link up | property test: random submit sequences, 10,000 iterations |
| `prio` | `dm_irq_prio` for 2, 3, 4, 8 bits; never 0; zero-latency offset; init asserts fire for each bad configuration | table-driven |

**Integration (native\_sim, two fake links)**: 60 s run at 5× nominal load with one link flapping every 7 s and the other 10% lossy; assert C0 delivered 100%, C1 freshest-wins, C4 zero bytes on the metered link, no slab leak (`k_mem_slab_num_used_get` returns to 0), scheduler max round < 50 µs host time.

**Hardware labs (nRF52840 DK, any SPI transceiver with a done pin, logic analyser)**

1. **Interrupt-to-tx latency.** `CONFIG_DM_GPIO_PROBES=y`. Measure pin A to pin B with the BLE host idle, then with a vendor radio stack advertising at 20 ms. Record p50/p99. Expected: tens of µs idle; up to one connection event when the stack is busy. This is the number that justifies the priority table in section 11.
2. **sw-EDCA.** Two DKs, both C4 at the same period, no jitter: count collisions via CRC failures. Enable `DM_EDCA`: collisions should drop by an order of magnitude; C0 injected during the storm should see median wait < C0 `cw` max.
3. **Tier 0 ordering.** `DM_TIER_HW_NRF=y`, C0 on EGU0 at level 1, C4 on EGU1 at level 3. Trigger both completions within 1 µs using PPI from one TIMER. Assert via trace that the C0 ISR runs first every time, and that `GPIOTE_IRQn` never fires for these.
4. **Airtime accounting.** Compare `stats.cls[c].airtime_ms_used` against logic-analyser-measured TX-enable time over 1,000 frames. Expected error < 2%.

**Static checks**: `-Wall -Wextra -Werror`, `CONFIG_ASSERT=y` in tests, `CONFIG_THREAD_ANALYZER=y` run in the hardware sample to prove stack sizes, `west build -t compliance`, and no symbol from `k_malloc`, `k_work_submit` (system queue) or `printk` in `subsys/dockmaster/` (checked by a `grep` step in CI).

## 16. Implementation milestones

Eight milestones, each a mergeable PR with its own tests green on `native_sim`. The agent works them in order; each line is one task it can take without further instruction. Hardware is needed only from M6.

- [ ] **M0: Skeleton (half a day).** `module.yml`, `Kconfig` with every option from section 14, `CMakeLists.txt`, empty headers with the full API from section 5 and doxygen on every symbol, `dm_init()` returning 0, `samples/two_links_native` that builds and prints "dockmaster up". CI workflow: build native\_sim + mps2/an385.
- [ ] **M1: Queues (1 day).** `dm_queue.c` per section 7, `dm_submit`, `dm_alloc`/`dm_submit_block`, shed, coalesce, age, DUP refcnt, ISR path. `tests/unit/queue` complete. No scheduler yet; a test drains queues directly.
- [ ] **M2: Fake link + owner threads (1 day).** `dm_link.h` registration, `dm_link_fake.c` with latency/loss/busy knobs, `dm_owner.c` loop with watchdog, `dm_link_tx_done` path, `dm_prio.c` with `dm_irq_prio` and init asserts. `tests/unit/prio`. Sample sends one message per class through one fake link.
- [ ] **M3: Scheduler (1.5 days).** `dm_sched.c` per section 8: strict priority, buckets in µs, measured-airtime debit, C0 overdraw, k\_poll wake-ups, refill timer. `tests/unit/sched` including tick-wrap. Sample runs two fake links with the default policy.
- [ ] **M4: Policy + pressure (1 day).** `dm_policy.c` with validation, NVS two-slot, apply/revert, double-reboot auto-revert; `dm_pressure.c` state machine; `dm_hint`. `tests/unit/policy`, `tests/unit/pressure` including the 10,000-iteration property test. `dm_stats.c` and `dm_shell.c`.
- [ ] **M5: Link manager (1.5 days).** `dm_linkmgr.c` per section 10: EWMA, score, switch rule, dwell per class, flap decay, make-before-break, affinity, DUP on two links, EXCLUSIVE. `tests/unit/linkmgr`; `tests/integration/two_links` with the 60 s fault-injection run. **v0.1 tag here**: fully usable on any board with user-written drivers.
- [ ] **M6: sw-EDCA + SPI PHY driver (1.5 days).** `sw_edca_wait` in `dm_owner.c`, Kconfig AIFS/CW tables, `dm_link_spi_phy.c` generic transceiver driver with `is_busy`, `samples/dk_spi_phy_edca`, GPIO probe points, hardware labs 1 and 2 scripted with a README on how to measure.
- [ ] **M7: Tier 0 nRF backend (1 day).** `hw/dm_hw_nrf_egu.c` with PPI/DPPI + EGU per section 11, `dockmaster,hw-nrf-egu` binding, `bind_class_irq` in the SPI PHY driver, hardware lab 3 with a trace capture checked in. Support both nRF52 (PPI) and nRF53 (DPPI) behind one API.
- [ ] **M8: Remaining drivers + docs (1 day).** `dm_link_onchip_ieee802154.c`, `dm_link_ncp_uart.c`, `nucleo_l476rg` build in CI, tracing hooks, `doc/` rendered with Sphinx matching Zephyr's style, three ADRs (airtime units, single scheduler thread, TX-only tier 0). **v0.2 tag.**

**Definition of done for every milestone**: all new code under `subsys/dockmaster/` has no `k_malloc`, no system-workqueue submit, no `printk`; every public function has a ztest; `CONFIG_ASSERT=y` passes; the sample's stack high-water marks are recorded in the PR description from `thread_analyzer`.

## 17. Non-goals and open questions

**Not in v1**: link aggregation or striping; fragmentation and reassembly (messages are ≤ one slab block); FEC or retransmission policy (the driver's concern); IP, sockets, TLS; receive-side classification (RX arrives through the driver's own path and is outside Dockmaster); power management beyond not waking owner threads when idle; multi-core (nRF53 app+net split is treated as one link whose MAC is a co-processor).

**Open questions, decide before M3**

- [ ] Should C2 outrank C1 on the medium (as mapped in section 9) while C1 outranks C2 in the queue? Current answer yes, because stale state is coalesced anyway; confirm with a media producer.
- [ ] Bucket units: µs internally and ms in the policy. Is 16-bit `rate_ms_per_s` enough for a fast on-chip link (max 65 s of air per second is plenty), and is `burst_ms` at 16 bits enough for C4 on a slow link (65 s of burst)? Likely yes; make them `uint32_t` if a reviewer objects.
- [ ] `dm_submit` from ISR returns `-EAGAIN` on lock contention. Alternative is a lock-free SPSC ring per class for the ISR path. Defer unless lab 1 shows contention.
- [ ] Policy transport: Dockmaster only accepts a `struct dm_policy`. Who serialises it over the air and signs it is the application's problem, but a reference CBOR encoder in `samples/` would help adoption.
- [ ] Name: "Dockmaster" has no known Zephyr or NCS collision; verify on GitHub and crates.io/PyPI before the first public tag.

**Relationship to Zephyr `net_tc` and `conn_mgr`**: both stay untouched. A later adapter could expose a Dockmaster link as a `net_if` so IP traffic from `net_tc` queues flows through the same scheduler, and `conn_mgr` events could feed `dm_link_state`. Neither is required for v1 and both are out of scope until the core is proven on hardware.
