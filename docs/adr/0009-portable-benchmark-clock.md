# ADR-0009: Portable benchmark clock and startup self-check

- Status: Accepted
- Date: 2026-08-08

## Context

The measurement contract requires a disclosed monotonic source, overhead treatment, and defensible resolution before benchmark percentiles can be published.
Clock APIs differ across architectures, and a nominal duration unit does not establish effective timer granularity.
TSC measurements can also cross logical CPUs whose counters or AUX identities make an interval invalid.

## Decision

Measurement code lives in the benchmark-only `matching_engine::measurement` library and never links into `matching_engine::core`.
On x86-64, runtime CPUID selection requires extended leaf `0x80000001` EDX bit 27 for `RDTSCP` and leaf `0x80000007` EDX bit 8 for invariant TSC.
Missing leaves or features select the steady-clock fallback before any `RDTSCP` instruction can execute.
Selected TSC endpoint reads use `LFENCE; RDTSCP; LFENCE`, capture `TSC_AUX`, and discard intervals whose AUX values differ.
Other supported platforms and unqualified x86 processors use steady-clock nanosecond units as a portable fallback.
The fallback's exact unit conversion is not represented as one-nanosecond effective resolution.
The fallback is always regression-only and never publication-capable.

A startup self-check measures paired-reader overhead, zero deltas, backward reads, migration discards, and the smallest observed nonzero delta.
Zero deltas remain in the distribution, and more than 90 percent zero deltas among valid samples fails clock safety.
Median and p99 use nearest rank, including the lower middle observation for even-sized p50 samples.
The x86 TSC is calibrated against steady clock across multiple bounded sleep-based windows.
Each steady endpoint is bracketed by serialized TSC reads, and midpoint TSC values are used for calibration math.
Injected readers perform fixed work without polling, so a frozen reader reports calibration failure.
Calibration reports the median ticks-per-nanosecond ratio and the range divided by the median as uncertainty.
Fallback reports use exactly one tick per nanosecond, zero uncertainty, and explicitly state that no calibration occurred.
`calibrated` becomes true only after all windows and stability checks pass.

Clock safety refuses unsupported or non-steady sources, excessive invalid or zero samples, zero observable granularity, and failed or unstable calibration.
Publication is a separate decision represented by source capability, operation evaluation, and a publication reason.
Only a qualified x86 source with stable calibration and an evaluated operation median at least 10 times effective granularity is publication-capable by default.
Integer conversion validates finite, positive, and representable values before narrowing.

Linux arm64 PMU support and private macOS kperf access are deferred.
PMU policy and portability require a separate design, while private Apple frameworks are not an acceptable dependency.
Unsafe instruction probing through `SIGILL` recovery is outside this phase.

## Consequences

Benchmark startup can fail explicitly instead of silently producing attractive but unresolved percentiles.
x86 measurements gain serialized endpoint reads and migration detection.
macOS arm64 and unqualified x86 systems can pass clock safety, but their fallback results remain explicitly non-publishable and regression-only.
The measurement library may use floating point for calibration while the matching core remains integer-only.
Open-loop generation and histogram storage remain future work.
