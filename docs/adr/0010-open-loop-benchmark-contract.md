# ADR-0010: Open-loop order-book benchmark contract

- Status: Accepted
- Date: 2026-08-08

## Context

Closed-loop request generation pauses arrivals while the system is busy.
That coordinated omission can hide queueing delay and produce attractive but invalid tail latency.
Benchmark setup can also contaminate results if order allocation, maker insertion, schedule generation, or artifact I/O occurs inside the timed region.
A high dynamic-range distribution is required to retain tails without storing one value per event.

## Decision

The engine benchmark uses precomputed open-loop intended arrivals.
Checked rational arithmetic converts an explicit requested rate and calibrated tick ratio into absolute tick offsets without iterative drift.
An early event waits until its intended arrival.
A late event starts immediately.
Latency is measured from intended arrival through completion.
Actual-start lateness and event backlog are separate observations.
Backward and migrated intervals are discarded and counted.

Maker liquidity, schedules, book capacity, trade output, and histograms are fully allocated before warmup and measurement.
`crossing-limit` consumes one maker per event.
`sweep-3-level` consumes one maker at each of three disjoint ascending levels per event.
Every result and trade is validated and folded into a checksum.

HdrHistogram_c is a benchmark-only dependency pinned to commit `18c7a324383dded1451d15621cd018b0048057d0`.
Open-loop values use raw recording only.
A separate closed-loop synthetic diagnostic may call coordinated-omission correction with an explicit expected interval.
Diagnostic raw and corrected artifacts are named separately and cannot support engine claims.

Each run writes recorded-bucket CSV, percentile text, and schema-versioned strict JSON.
Clock safety is mandatory.
Source and operation publication qualification are separate.
Failure of the 10x effective-granularity rule produces successful local `regression_only` artifacts, while an unsafe clock fails the run.
The macOS steady fallback is permanently regression-only.

## Consequences

Open-loop tails include scheduler delay and accumulated queueing rather than omitting blocked arrivals.
Rate sweeps require explicit user-selected rates and do not infer saturation.
The three-level workload uses a larger price domain because shared preloaded best levels would violate the requirement that every event touch exactly three levels.
HdrHistogram never enters the core target's link interface or runtime dependency graph.
Artifacts can support review and regression investigation, but `publishable_candidate` still requires the environment disclosures in ADR-0002 before any publication.
