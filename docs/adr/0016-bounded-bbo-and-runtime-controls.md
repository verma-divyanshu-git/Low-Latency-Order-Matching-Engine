# ADR-0016: Bounded BBO snapshots and caller-owned runtime controls

- Status: Accepted
- Date: 2026-08-28

## Context

Readers need a current best bid and offer without locking the matching owner.
Applications also need optional CPU affinity and memory preparation without embedding host policy or thread ownership in pipeline stages.

## Decision

`BboSnapshot` publishes a complete `BboState` under an odd/even sequence counter.
Readers retry a bounded number of times and return no state when a consistent snapshot is unavailable.
All version and payload operations use sequentially consistent ordering so a successful reader cannot combine fields from separate publishes on weak-memory hosts.

`pin_current_thread` accepts caller-supplied CPU IDs and reports invalid input, unsupported platforms, and system failures explicitly.
`prepare_memory` prefaults caller-owned storage and optionally requests memory locking.
Neither control creates threads or changes pipeline ownership.

## Consequences

BBO readers are lock-free from the caller's perspective and have a fixed retry bound.
Publication failure is visible to callers instead of silently returning a partial state.
CPU placement, privilege, and memory-lock policy remain application concerns.
The runtime core remains portable and has no platform dependency outside the control implementation.

## References

- [C++ working draft: atomics.order](https://eel.is/c++draft/atomics.order)
- [Linux sched_setaffinity](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
- [Linux mlock](https://man7.org/linux/man-pages/man2/mlock.2.html)