# ADR-0007: Allocation-free structural invariant diagnostics

## Status

Accepted.

## Context

The order book deliberately stores redundant representations of the same state.
Price-level metadata, occupancy bitmaps, intrusive order links, encoded side and level fields, and arena liveness must agree.
Checking only public BBO and aggregate values can miss structural damage that affects later cancellation, matching, or slot reuse.

The diagnostic is intended for tests, replay verification, and debug tooling.
It must not allocate or throw after `OrderBook` construction, and it must not add work to normal release order operations.

## Decision

`OrderBook::check_invariants()` walks both sides and every configured price level in a fixed order.
It cross-checks occupancy, empty-level metadata, list endpoints, reciprocal links, encoded ownership, positive remaining quantity, counts, aggregates, arena liveness, total reachability, and the uncrossed BBO condition.
It validates each occupancy bitmap's summary hierarchy before any BBO traversal, while bitmap traversal independently bounds every derived word index.

The book allocates one `uint32_t` visit-mark array at arena capacity during construction.
Each check advances an epoch so normal checks do not clear the array.
Epoch wrap clears marks with a typed index loop before restarting at epoch one, including safe handling of zero capacity.

The checker returns a fixed trivially-copyable result containing an enum and numeric side, level, order, and reachability context.
Checks use a documented deterministic traversal and return the first violation.
Invalid indexes and dead slots are reported before any order dereference.

The checker is called explicitly.
Release submission, cancellation, amendment, and replacement paths do not invoke it.

## Consequences

The diagnostic catches disagreement among the order book's redundant structural representations without dynamic allocation during a check.
Its first-failure result can be compared directly across deterministic replays without allocating strings.
The checker performs a full scan of configured levels and arena capacity, so callers choose when that diagnostic cost is appropriate.

The checker does not prove global submitted, executed, and canceled volume conservation.
It also does not prove self-trade prevention.
Those properties belong to the Phase 2 reference model and future STP tests.
