# ADR 0004: Class queues are rings, not k_msgq

Status: accepted

## Context

The spec asked for one `k_msgq` per class holding 8-byte references, and for
C1 coalescing to replace an older queued message in place (same queue
position, new length and tick). `k_msgq` offers put, get and peek but no way
to modify or remove an entry in the middle; `k_msgq_put_front` only arrived
in Zephyr 4.x and does not help either.

## Decision

Each class queue is a fixed ring of the same 8-byte `struct dm_msg_ref`
with the depth from Kconfig. Every ring operation is O(1) (or a bounded
shift for the rare middle removal) under a short `irq_lock`. Coalescing
writes the new payload into a fresh block, then swaps the reference at the
old position under the lock and frees the old block, so the scheduler can
never see a half-written payload. Producers only append; popping needs a
consumer flag taken with try semantics, so no context ever waits.

## Consequences

- Coalesce keeps queue position and is safe against a concurrent dispatch.
- The ISR path stays one slab allocation, one copy and one append.
- The per-link dispatch queue and the completion queue are still `k_msgq`,
  where its blocking semantics are exactly what is wanted.
- About 60 lines of ring code that the queue suite covers directly.
