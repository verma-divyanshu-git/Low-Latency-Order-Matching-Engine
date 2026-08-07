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
It exits nonzero when the clock fails the self-check.
The accepted ranges are 1 through 1,000,000 samples and 1 through 10,000 calibration milliseconds.

## Sources

On x86-64, each endpoint uses `LFENCE; RDTSCP; LFENCE`.
`RDTSCP` returns `TSC_AUX`, and an elapsed sample is discarded as a migration when endpoint AUX values differ.
The TSC is calibrated against `std::chrono::steady_clock` over five bounded windows.
The report uses the median ticks-per-nanosecond ratio and reports full-range spread divided by that median as calibration uncertainty.

Other supported targets, including macOS arm64, use `std::chrono::steady_clock` converted to nanosecond units.
One fallback tick is therefore one represented nanosecond, but this says nothing about the clock's effective resolution.
The self-check reports the smallest observed nonzero delta as effective granularity.
It does not claim that the nominal nanosecond unit provides one-nanosecond granularity.

Linux arm64 PMU access is deferred because availability, privilege, frequency behavior, and userspace access policy need a separate design.
Private macOS kperf interfaces are also deferred because they are unsupported private frameworks.
This phase does not probe instructions with `SIGILL` handlers.

## Refusal criteria

The report is not publishable when the source is unsupported or non-steady, more than 10 percent of requested samples are backward or migration-discarded, no nonzero granularity is observed, or x86 calibration uncertainty exceeds 5 percent.
An operation report must also be rejected when its median is below the configured resolution multiple, which defaults to 10 times effective granularity.
Failure reports retain counters and a reason enum but set `percentiles_publishable` to false.

The fallback ratio is exactly one tick per nanosecond and `calibrated` is false.
It carries no invented calibration precision.
Local macOS fallback results are suitable for regression detection on the same controlled machine only.
They are not portable latency claims because effective timer granularity, scheduling, power management, and hardware differ.

## Scope

`matching_engine::measurement` is a separate library built only when `ENGINE_BUILD_BENCHMARKS=ON`.
`matching_engine::core` does not link it and remains free of runtime dependencies and floating-point measurement code.
Open-loop load generation and latency histograms are intentionally deferred.
