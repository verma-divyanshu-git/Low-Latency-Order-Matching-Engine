# Measurement clock self-check

Phase 3A adds clock validation infrastructure, not an engine benchmark.
It does not publish matching latency or throughput.

## Build and run

```sh
cmake --preset measurement
cmake --build --preset measurement
ctest --preset measurement
./build/measurement/clock_probe --samples 10000 --calibration-ms 10
```

`clock_probe` writes one deterministic JSON object to stdout and a short diagnostic to stderr.
It exits zero when `clock_safe` is true, even when the source is regression-only and no latency percentiles are publishable.
It exits nonzero when clock safety fails.
The accepted ranges are 1 through 1,000,000 samples and 1 through 10,000 calibration milliseconds.

A fallback result has the following semantic shape.
The numeric observations vary by run and are not latency claims.

```json
{"source":"steady_clock_ns","source_publication_capable":false,"calibrated":false,"clock_safe":true,"source_publishable":false,"operation_evaluated":false,"operation_percentiles_publishable":false,"self_check_reason":"clock_safe","publication_reason":"source_regression_only"}
```

Compiler metadata is emitted as a fixed family enum and numeric major, minor, and patch fields.
No free-form compiler version string is interpolated into JSON.

## Sources

On x86-64, startup first queries the maximum extended CPUID leaf.
It selects `RDTSCP` only when leaf `0x80000001` reports EDX bit 27 and leaf `0x80000007` reports invariant TSC in EDX bit 8.
If any leaf or bit is absent, selection falls back to steady clock without executing `RDTSCP`.
Each selected TSC endpoint uses `LFENCE; RDTSCP; LFENCE`.
`RDTSCP` returns `TSC_AUX`, and an elapsed sample is discarded as a migration when endpoint AUX values differ.
The TSC is calibrated against `std::chrono::steady_clock` over five bounded sleep-based windows.
Each steady timestamp is bracketed by two serialized TSC reads, and the corresponding TSC midpoint removes one-sided endpoint ordering bias.
Injected readers consume exactly two steady reads per window and never poll, so frozen fake clocks fail in bounded work.
The report uses the median ticks-per-nanosecond ratio and reports full-range spread divided by that median as calibration uncertainty.

Other supported targets, including macOS arm64, use `std::chrono::steady_clock` converted to nanosecond units.
One fallback tick is therefore one represented nanosecond, but this says nothing about the clock's effective resolution.
The self-check reports the smallest observed nonzero delta as effective granularity.
It does not claim that the nominal nanosecond unit provides one-nanosecond granularity.

Linux arm64 PMU access is deferred because availability, privilege, frequency behavior, and userspace access policy need a separate design.
Private macOS kperf interfaces are also deferred because they are unsupported private frameworks.
This phase does not probe instructions with `SIGILL` handlers.

## Refusal criteria

Clock safety and operation publication are separate decisions.
`clock_safe` requires a supported steady source, at most 10 percent backward or migration-discarded samples, at most 90 percent zero deltas among valid samples, observable nonzero granularity, and stable calibration when TSC is selected.
Zero deltas remain in the overhead distribution, while the smallest nonzero delta defines effective granularity.
A zero median therefore never implies sub-granularity precision.

The overhead median and p99 both use nearest rank: sort ascending, use rank `ceil(p * N)`, and convert the one-based rank to an index.
For an even-sized sample, p50 is therefore the lower middle observation.

Steady-clock fallback always sets `source_publishable` to false and `publication_reason` to `source_regression_only`.
A qualified x86 source still cannot publish operation percentiles until `operation_evaluated` is true.
The operation median must be at least the configured resolution multiple, which defaults to 10 times effective granularity.
Only then is `operation_percentiles_publishable` true with publication reason `qualified`.
Clock failures retain observations and use a `self_check_reason` distinct from `publication_reason`.

The fallback ratio is exactly one tick per nanosecond and `calibrated` is false.
It carries no invented calibration precision.
Local macOS fallback results are suitable for regression detection on the same controlled machine only.
They are never publishable latency evidence because effective timer granularity, scheduling, power management, and hardware differ.

## Scope

`matching_engine::measurement` is a separate library built only when `ENGINE_BUILD_BENCHMARKS=ON`.
`matching_engine::core` does not link it and remains free of runtime dependencies and floating-point measurement code.
Open-loop load generation and latency histograms are intentionally deferred.
