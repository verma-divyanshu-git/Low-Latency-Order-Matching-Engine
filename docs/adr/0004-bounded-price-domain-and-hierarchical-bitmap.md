# ADR-0004: Bounded price domain and hierarchical occupancy bitmap

- Status: Accepted
- Date: 2026-08-08

## Context

The order book needs to map integer prices to dense level indexes and find populated levels without allocation after startup.
This slice does not need an unbounded or sparse price universe.

## Decision

Startup configuration defines an inclusive minimum price and a positive 32-bit tick count.
The resulting contiguous domain must fit in signed 64-bit ticks without overflow.
Prices outside that configured domain have no index and must be rejected by callers before book mutation.
Configurations that require a wider universe, sparse price bands, or gaps are unsupported and must be rejected rather than silently compressed.

Level occupancy uses a fixed hierarchical bitmap.
The base level records populated price levels, and each higher level records which words in the level below are nonempty.
Construction derives levels until one summary word remains, checks size arithmetic, and allocates all storage once.
Mutation and lookup after construction are allocation-free.
The hierarchy guides lookup across empty base words rather than scanning each one.

Operations are constant time only with respect to a configured bounded universe whose maximum hierarchy depth is fixed by the 32-bit index range.
They are not constant time for an arbitrary unbounded price universe.

## Alternatives

`std::map` supports sparse and unbounded prices, but each level carries node and pointer overhead and ordinary insertion may allocate.
A sorted vector has compact storage and good iteration locality, but insertion and removal move elements and populated-level lookup is logarithmic.
QuantCup-style linear scanning keeps a direct price array simple, but discovery work grows with every empty base word between populated levels.
The hierarchical bitmap retains direct bounded indexing while skipping empty word ranges through summaries.

## Consequences

The configured price interval is explicit and validated before matching starts.
Memory use is proportional to the full configured tick universe, including unpopulated prices.
The bitmap can discover first, last, next, and previous populated levels without linear empty-word scans.
Workloads requiring very wide or sparse prices need a different representation in a future decision.
