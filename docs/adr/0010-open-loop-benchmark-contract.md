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
Checked rational arithmetic fully cancels factors and converts an explicit requested rate and calibrated tick ratio into nearest-rounded absolute tick offsets without iterative drift or floor bias.
An early event waits until its intended arrival.
A late event starts immediately.
On x86, the schedule-base AUX defines the only CPU accepted for the run.
Any different AUX during waiting, at immediate start, or at completion aborts the run before another event executes and prevents successful artifact publication.
The harness does not rebase after migration because invariant TSC does not establish cross-CPU counter synchronization.
Publishable x86 candidates require documented OS-level CPU pinning.
Latency is measured from intended arrival through completion.
Actual-start lateness and additional already-arrived events behind the current event are separate harness observations, not engine queue depth.
Backward intervals are discarded and counted.

Maker liquidity, schedules, book capacity, trade output, raw observations, bounded result/trade captures, and histograms are fully allocated before warmup and measurement.
The benchmark rejects checked memory plans above 256 MiB rather than weakening the core's caller-capacity contract.
`crossing-limit` consumes one maker per event.
`sweep-3-level` consumes one maker at each of three disjoint ascending levels per event.
Completion is captured immediately after submission returns.
The arrival loop performs only waiting, immediate clock reads, submission, and a fixed copy of the result and at most three trades.
Every captured result and trade is validated and folded into a checksum after all arrivals complete.
Histogram population, conversion, lateness, and harness arrival-backlog calculations also happen after the arrival loop.
The bounded post-completion copy and other single-thread driver work can delay later arrivals, so reported lateness and backlog include generator overhead.
Achieved completion rate uses all executed operations and the first-to-last completion interval across `N - 1` intervals.

HdrHistogram_c is a benchmark-only dependency pinned to commit `18c7a324383dded1451d15621cd018b0048057d0`.
Open-loop values use raw recording only.
A separate closed-loop synthetic diagnostic may call coordinated-omission correction with an explicit expected interval.
Checked preflight rejects diagnostic configurations that can exceed 10,000,000 corrected records.
Diagnostic raw and corrected artifacts are named separately and cannot support engine claims.
Their top-level claim scope is `diagnostic_only`, while nested operation qualification remains unevaluated and nonpublishable.

Each run writes recorded-bucket CSV, full Hdr percentile-iteration text with highest-equivalent-value semantics, and schema-versioned strict JSON.
Successful open-loop JSON requires finite doubles, at least one valid latency sample, internally consistent valid/invalid accounting, and zero migration samples.
Files are checked in a unique staging directory and atomically renamed as one final run directory.
Existing final directories are never overwritten.
Clock safety is mandatory.
Source and operation publication qualification are separate.
Operation resolution is evaluated even when the source itself is regression-only, and JSON retains both refusal reasons plus quantization fields.
Failure of the 10x effective-granularity rule produces successful local `regression_only` artifacts, while an unsafe clock fails the run.
The macOS steady fallback is permanently regression-only.

## Consequences

Open-loop tails include scheduler delay, single-thread generator overhead, and accumulated waiting rather than omitting blocked arrivals.
Rate sweeps require up to 32 unique explicit user-selected rates and do not infer saturation.
The three-level workload uses a larger price domain because shared preloaded best levels would violate the requirement that every event touch exactly three levels.
HdrHistogram never enters the core target's link interface or runtime dependency graph.
Artifacts can support review and regression investigation, but `publishable_candidate` still requires the environment disclosures in ADR-0002 before any publication.
