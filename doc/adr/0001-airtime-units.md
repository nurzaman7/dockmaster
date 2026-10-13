# ADR 0001: Budgets are airtime, not bytes

Status: accepted

## Context

A device with an 802.15.4 radio at 250 kbit/s, a sub-GHz PHY at a few
kbit/s and a cellular modem has no common byte rate. A byte budget that is
generous on one link starves another, and regulatory limits (duty cycle,
dwell time) are stated in time, not bytes. Half-duplex radios also go deaf
while they transmit, which costs time no byte count shows.

## Decision

Every budget is in milliseconds of air per second (`rate_ms_per_s`,
`burst_ms` in the policy), kept as microseconds internally. Each driver
supplies `cost_of_us(len)`, a pure airtime model including preamble and
guard time. The scheduler reserves the estimate at dispatch and, on
completion, charges what the driver measured (or what Dockmaster measured
between `tx` and `tx_done`). Half-duplex links report airtime including the
receive-off time.

## Consequences

- One policy works across links of very different speed: a fast link moves
  more bytes inside the same milliseconds.
- A duty-cycle limit is just a bucket rate (1 % = 10 ms/s).
- Estimates may drift; measurements correct them every message. The `sched`
  suite asserts that the measured value is what ends up charged.
- Drivers must provide an honest `cost_of_us`; a driver that underestimates
  gets a link that is busier than its budget for one message at most.
