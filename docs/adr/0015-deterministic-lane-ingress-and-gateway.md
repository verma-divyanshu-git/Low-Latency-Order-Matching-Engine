# ADR-0015: Deterministic lane ingress and gateway validation

- Status: Accepted
- Date: 2026-08-28

## Context

Multiple producers must submit commands without making engine order depend on scheduler timing.
Ingress also needs to reject unsafe requests before sequencing or matching changes state.

## Decision

Each producer owns one bounded SPSC lane.
`LaneCommand` carries caller-supplied logical time, `LaneId`, and per-lane sequence.
The merger orders visible lane heads by logical time, then lane ID, then lane sequence.
It does not read a clock or own producer threads.

`GatewayValidator` owns the active client order-ID set before sequencing.
It rejects zero or duplicate IDs, active-order capacity exhaustion, excessive quantity or checked integer notional, price-collar violations, invalid lanes, and per-lane rate-limit breaches.
Each configured lane has an independent fixed logical-time rate window.
The legacy overload maps callers without an explicit lane to lane zero.

## Consequences

Arrival permutations that preserve each lane's FIFO order produce the same merged sequence after the same lane heads are visible.
An earlier visible command cannot be starved by a busy lane with later logical times.
The engine keeps no client-ID map and receives only gateway-accepted commands.
The gateway uses bounded storage established at construction.

## References

- [C++ working draft: data races](https://eel.is/c++draft/intro.races)
- [Rigtorp SPSCQueue](https://github.com/rigtorp/SPSCQueue)