# ADR 0002: One scheduler thread for all links

Status: accepted

## Context

The alternatives were a scheduler per link, scheduling inside each owner
thread, or scheduling on the system workqueue. Class priority, buckets,
DUP (one message on two links) and the link manager's per-class choice all
need a consistent view across links.

## Decision

A single preemptible thread (`dm_sched`, priority 4) owns every scheduling
decision: completions, link scores, age and shed, and dispatch. It wakes on
one `k_poll` signal raised by submits into an empty class, completions,
link events and a refill timer that runs only while work is pending. Owner
threads only transmit; they never choose what to send. Nothing runs on the
system workqueue.

## Consequences

- No locks between schedulers; DUP and failover are simple because one
  thread sees every link.
- A round costs at most 5 classes x N links x one `cost_of_us` call plus the
  number of messages it sheds or ages out.
- One thread is a serialisation point. At the rates these radios run
  (hundreds of frames per second at most) that is not the limit; the round
  time is measured in `dm_stats.sched_max_us` so it can be watched.
- Tests can drive the same code synchronously (`dm_sched_round(now)` with a
  fake clock), which is how every scheduler rule is tested deterministically.
