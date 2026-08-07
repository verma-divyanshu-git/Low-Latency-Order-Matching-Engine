# ADR-0011: Host qualification and noisy-CI regression gate

- Status: Accepted
- Date: 2026-08-08

## Context

Clock qualification and a sound open-loop workload are necessary but insufficient for publishable low-latency evidence.
CPU affinity, isolation, frequency policy, interrupts, kernel housekeeping, memory policy, virtualization, and idle-window noise can materially change results.
The project also needs an inexpensive regression signal on free shared CI runners, whose environment cannot satisfy those publication controls.
Treating shared-runner batch throughput as a latency result or a publishable throughput claim would violate the measurement contract.

## Decision

Every publishable candidate is coupled to a schema-versioned Linux host-qualification report collected for that candidate.
The verifier is report-only and never tunes the machine.
It uses only the Python standard library and checked procfs and sysfs parsing.
Every required check must pass.
Missing architecture-specific advisory evidence is recorded as unavailable without guessing.
Missing required evidence makes the report nonqualified.
Non-Linux invocations still write a valid nonqualified report and fail.
Reports omit host identity and other sensitive machine identifiers.
Host-qualification schema version 2 makes `evidence_mode` mandatory on every report.
Only `live_host` evidence can qualify.
Any fixture-root run is permanently labeled `fixture`, forced nonqualified, and exits nonzero even if all synthetic checks pass.
Fixture reads canonicalize the root once, confine every resolved mapped path beneath it, and reject every symlink component.
This read-only validation does not claim to eliminate filesystem TOCTOU races.
Clocksource qualification uses an explicit architecture allow-list and requires the selected nonempty source in the nonempty available-source list.
The accepted sources are `tsc` on x86 and `arch_sys_counter` on Linux arm64 and aarch64.

A separate `order_book_throughput_gate` executable measures only a fixed crossing-limit batch.
Construction and preload happen before each timed region.
Validation and checksum folding happen after timing.
The gate performs 3 through 21 bounded repetitions and reports elapsed durations, minimum elapsed duration, median, integer median absolute deviation, relative MAD, and maximum batch-amortized throughput.
It reports no operation histogram and no latency percentiles.
Its scope is permanently `ci_regression_only`.

The CI decision uses min-of-N throughput because occasional shared-runner interruption should not hide gross engine regressions.
This statistic is optimistically biased and cannot support a sustained-throughput claim.
Dispersion is independently gated with relative MAD and always reported.
The initial floor is the deliberately conservative policy value of 1,000,000 operations per second.
The initial relative-MAD cap is 25 percent.
Any later widening must document public-runner flakiness rather than silently weakening the signal.

The Ubuntu Release job uses a pinned checkout action and distribution build tools.
It does not require PMU access, upload percentile artifacts, or describe its output as publishable.

## Consequences

Publishable candidates carry evidence for the Linux environment that produced them instead of borrowing a generic host description.
Operators must configure hosts explicitly before verification.
Architecture and kernel differences remain visible as unavailable evidence rather than fabricated passes.
Free CI provides a bounded gross-regression detector but no resume number, latency claim, or comparable benchmark result.
Min-of-N and MAD definitions are deterministic and unit-tested, including zero, overflow, threshold, and finite-JSON boundaries.
