# ADR-0009: Portable benchmark clock and startup self-check

- Status: Accepted
- Date: 2026-08-08

## Context

The measurement contract requires a disclosed monotonic source, overhead treatment, and defensible resolution before benchmark percentiles can be published.
Clock APIs differ across architectures, and a nominal duration unit does not establish effective timer granularity.
TSC measurements can also cross logical CPUs whose counters or AUX identities make an interval invalid.

## Decision

Measurement code lives in the benchmark-only `matching_engine::measurement` library and never links into `matching_engine::core`.
On x86-64, endpoint reads use `LFENCE; RDTSCP; LFENCE`, capture `TSC_AUX`, and discard intervals whose AUX values differ.
Other supported platforms use steady-clock nanosecond units as a portable fallback.
The fallback's exact unit conversion is not represented as one-nanosecond effective resolution.

A startup self-check measures paired-reader overhead, zero deltas, backward reads, migration discards, and the smallest observed nonzero delta.
It reports fixed min, median, and p99 overhead fields only as publishable after all refusal checks pass.
The x86 TSC is calibrated against steady clock across multiple bounded windows.
Calibration reports the median ticks-per-nanosecond ratio and the range divided by the median as uncertainty.
Fallback reports use exactly one tick per nanosecond, zero uncertainty, and explicitly state that no calibration occurred.

The self-check refuses unsupported or non-steady sources, excessive invalid samples, zero observable granularity, unstable calibration, and operation medians below 10 times effective granularity by default.
Integer conversion validates finite, positive, and representable values before narrowing.

Linux arm64 PMU support and private macOS kperf access are deferred.
PMU policy and portability require a separate design, while private Apple frameworks are not an acceptable dependency.
Unsafe instruction probing through `SIGILL` recovery is outside this phase.

## Consequences

Benchmark startup can fail explicitly instead of silently producing attractive but unresolved percentiles.
x86 measurements gain serialized endpoint reads and migration detection.
macOS arm64 remains safe and portable, but local fallback results are regression-only and cannot support cross-machine publication claims.
The measurement library may use floating point for calibration while the matching core remains integer-only.
Open-loop generation and histogram storage remain future work.
