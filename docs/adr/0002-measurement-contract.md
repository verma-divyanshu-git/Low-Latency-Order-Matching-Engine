# ADR-0002: Measurement contract

- Status: Accepted
- Date: 2026-08-08

## Context

Low-latency results are sensitive to workload shape, clocks, compiler settings, hardware topology, operating-system noise, warm-up, and the boundary of the timed operation.
A single average or best run cannot establish predictable behavior.
Publishing numbers without enough reproduction data would encourage invalid comparisons.

## Decision

Every published performance result must identify the exact source revision, compiler and flags, hardware, operating system, and relevant runtime configuration.
Latency and throughput experiments must be separate unless the experiment explicitly studies their trade-off.
Latency reports must include a distribution with sample count and tail percentiles.
Reports must define the workload, warm-up, duration, queue depth, measurement boundary, clock source, and overhead treatment.
Reports must disclose thread affinity, CPU isolation, frequency policy, simultaneous multithreading, NUMA placement, and relevant background load.
Raw observations and the scripts required to reproduce published summaries must be retained.
Comparisons are permitted only when systems are measured under an equivalent contract and limitations are stated.
No benchmark result may be published from the Phase 0 foundation because no benchmark target exists.

## Consequences

Performance work requires more metadata and review than a single timing loop.
Results can be rejected when the environment or method is insufficiently controlled.
The project gains reproducible evidence and can distinguish regressions from environmental noise.
Some measurements will remain hardware-specific and must be described as such.

## References

- [Google Benchmark user guide](https://google.github.io/benchmark/user_guide.html)
- [Linux perf documentation](https://perf.wiki.kernel.org/index.php/Tutorial)
- [Intel invariant time-keeping](https://www.intel.com/content/www/us/en/developer/articles/technical/invariant-time-keeping.html)
- [Project references](../references.md)
