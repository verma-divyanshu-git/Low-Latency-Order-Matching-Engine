# ADR-0029: Equivalent price-index comparison benchmark

## Status

Accepted.

## Decision

Measured optimization starts with an isolated ordered price-level workload rather than four partial matching-engine rewrites.
The production hierarchical bitmap and dense quantity array are compared with `std::map`, a reserved sorted vector, and pinned Abseil `btree_map` under identical keys, quantities, operations, preloading, repetitions, and checksum validation.

The benchmark reports median batch-amortized throughput and retains every repetition duration.
It is always regression-only and makes no operation-latency or full-engine claim.
Abseil remains a private benchmark-only dependency.

## Consequences

The comparison can isolate branch, cache, movement, and allocation hypotheses without weakening matching semantics or adding dependencies to production targets.
It cannot predict full matcher performance by itself.
An optimization is accepted only after the full engine benchmark and correctness gates confirm it.
