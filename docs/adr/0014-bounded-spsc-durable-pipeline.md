# ADR-0014: Bounded SPSC durable matching pipeline

- Status: Accepted
- Date: 2026-08-08

## Context

Durable commands must reach the existing single-writer engine without locks, hot-path allocation, sequence gaps, or engine mutation that cannot be fully published.
Thread creation and affinity policy must remain outside reusable stages.

## Decision

Use exact-capacity runtime-sized SPSC rings between three non-thread-owning stages.
Each ring uses producer-owned and consumer-owned monotonic unsigned indexes with acquire/release publication and cached remote indexes.
Producer and consumer mutable state is explicitly aligned to 128 bytes.
Move-only, single-claim endpoints make the intended one-producer and one-consumer ownership visible in the API.

Ingress follows check capacity, check journal, stamp, append, publish.
Any persistence failure after stamping permanently stops that ingress object.
Matching peeks at a command, preflights the engine's exact conservative event requirement, applies into fixed scratch, publishes the exact batch, and then releases input.
No queue reservation API is needed because only one producer can consume availability and consumer progress can only add capacity.
The producer instead exposes an all-or-nothing copy batch operation that initializes every slot and release-publishes the tail exactly once.
Matching construction rejects event queues smaller than the engine's maximum batch, making successful preflight followed by failed batch publication an invariant violation.

Snapshot persistence is permitted only on the matching owner thread between command-processing calls.
It may stall that thread.

## Consequences

The queue has no CAS loop, slot sequence number, lock, or allocation after construction.
Unsigned index arithmetic supports wrap while bounded occupancy prevents ambiguity.
Durable append precedes matcher visibility.
Ingress backpressure consumes no sequence.
Output backpressure leaves both command input and engine state untouched.
Publication callbacks must be non-throwing so callback failure cannot consume an event.
Persistence uncertainty and impossible apply mismatches stop their stage and require explicit recovery instead of skipping.

The queue is SPSC only.
Applications remain responsible for lifecycle ordering, waiting, CPU placement, and destruction after endpoint users stop.

## References

- [Rigtorp SPSCQueue](https://github.com/rigtorp/SPSCQueue)
- [1024cores bounded SPSC queue](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-spsc-queue)
- [C++ working draft: data races](https://eel.is/c++draft/intro.races)
