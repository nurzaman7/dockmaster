# ADR 0003: Hardware tier separates transmit completions only

Status: accepted

## Context

`DM_TIER_HW` routes per-class completion signals to separate NVIC lines so
a C0 completion preempts a C4 one in hardware (on nRF: GPIOTE to PPI or DPPI
to an EGU instance per class line). The question was whether to do the
same for received frames.

## Decision

Tier HW applies to transmit completions only. Before each transmission the
owner thread binds the radio's completion output to the line for that
message's class. Receive stays tier SW: the class of an incoming frame is
not known until its bytes are read over the bus, so there is nothing to
route by. A link may report `DM_TIER_HW` for TX while its RX path is
treated as SW automatically.

## Consequences

- Two lines cover five classes: C0 to C2 at `DM_IRQ_HW_C0`, C3 and C4 at
  `DM_IRQ_HW_C4`. Init asserts that lines are distinct and never at level 0.
- Receive-side classification stays out of scope (as the spec says).
- The benefit is bounded by the radio's ability to remap its interrupt
  output between transmissions; a radio without that falls back to SW.
