# Linux host qualification and CI throughput gate

Phase 3C completes the measurement infrastructure.
It does not publish a performance result.
It supplies environment evidence for Linux publishable candidates and a deliberately noisy batch-throughput regression signal for CI.

## Host qualification

Run the verifier after pinning the current process to the intended benchmark CPU.
The verifier never changes host configuration.
Tuning remains an explicit operator action.

```sh
taskset --cpu-list 2 scripts/verify-tuning.sh \
  --benchmark-cpu 2 \
  --sample-seconds 60 \
  --output benchmark-results/host-qualification.json
```

The output is schema-versioned strict JSON.
It contains the platform family and architecture, benchmark CPU, sample duration, qualification decision, and an ordered list of checks.
It intentionally excludes hostname, username, network addresses, serial numbers, and home-directory paths.
The process exits zero only when every required check passes.
Unavailable required evidence is a failure because missing evidence cannot support a publishable candidate.
Unavailable advisory evidence remains visible but does not independently fail qualification.
On non-Linux systems the command writes a valid nonqualified report and exits nonzero.

`--fixture-root` exists only for the fixture-driven test suite.
It must never be used to qualify a real host.
CLI integers are complete base-10 values, the sample duration is bounded from 1 through 300 seconds, and paths containing parent traversal are rejected.

## Check meanings

- `platform_linux` requires Linux because the evidence sources are Linux procfs and sysfs interfaces.
- `affinity_requested_cpu_only` requires the running verifier to have affinity to exactly the requested CPU.
- `cpu_online` confirms that the requested CPU is in the kernel online set.
- `clocksource_current` records the current kernel clocksource and requires TSC on x86.
- `clocksource_tsc_available`, `tsc_constant`, and `tsc_nonstop` require the x86 timing facilities used by the benchmark clock contract.
- `virtualization_disclosure` records whether the CPU flags disclose a hypervisor.
- `scaling_governor_performance` requires the performance governor.
- The minimum, maximum, current, and fixed-frequency checks require readable numeric controls, an in-policy current frequency, and equal policy endpoints.
- `turbo_policy_disclosure` records the exposed boost policy without assuming an architecture-specific sysfs file exists.
- `smt_sibling_isolation` requires no other online thread in the benchmark CPU's sibling set.
- `isolcpus`, `nohz_full`, and `rcu_nocbs` require the benchmark CPU in the corresponding kernel isolation sets.
- `irq_affinity_excludes_cpu` parses every exposed effective IRQ affinity and rejects any overlap.
- `nmi_watchdog_disabled` rejects periodic watchdog interruption on the benchmark CPU.
- `transparent_hugepages_disabled` and `swappiness_zero` reject two sources of memory-management variance.
- `aslr_disclosure` records the kernel ASLR policy but does not ask an operator to weaken it.
- `memory_lock_limit` requires an unlimited soft lock limit so benchmark memory can be faulted and locked by an explicit benchmark procedure.
- `perf_event_access` requires `perf_event_paranoid` at most 1 for later PMU collection.
- `pmu_device_access` requires the numeric CPU PMU event-source type on x86 and remains architecture-aware elsewhere.
- `numa_node_disclosure` records the benchmark CPU's NUMA node.
- `steal_time_delta`, `context_switch_delta`, and `interrupt_delta` use checked `/proc/stat` snapshots around a monotonic idle window, with steal time taken from the benchmark CPU row.
- Counter resets, wraps, malformed counters, more than one steal tick per second, or more than 100 context switches or interrupts per second fail the corresponding required check.
- Core and package throttling counters are advisory because many architectures and kernels do not expose the x86 thermal-throttle files.

CPU-list evidence is parsed with the kernel comma-and-range grammar.
Malformed lists, descending ranges, out-of-range identifiers, malformed counters, and counter resets fail instead of being guessed.
Architecture-specific files are reported unavailable where they do not apply.

## Coupling evidence to candidates

Every publishable benchmark candidate must retain the exact host qualification report collected for that candidate.
A report from another boot, CPU, kernel command line, or measurement window is not interchangeable evidence.
The benchmark summary, source revision, compiler and flags, raw observations, and qualification JSON form one review unit.
A benchmark artifact remains regression-only when its host report is absent or nonqualified even if its clock and operation-resolution checks pass.

Free shared runners cannot provide this evidence.
Their CPU model, neighbors, frequency policy, interrupts, virtualization, PMU access, and steal time are controlled by the provider and can change between jobs.
GitHub-hosted results therefore remain CI regression signals and must never be used on a resume, in comparative claims, or as latency evidence.

## Batch throughput CI gate

Build and run the separate gate in Release mode.

```sh
cmake --preset measurement
cmake --build --preset measurement --target order_book_throughput_gate
./build/measurement/order_book_throughput_gate \
  --samples 100000 \
  --repetitions 7 \
  --min-ops-per-second 1000000 \
  --max-relative-mad 0.25
```

Each repetition constructs and preloads a fresh crossing-limit book before timing.
The timed region submits one fixed batch.
Validation and checksum folding occur after timing.
The JSON reports each batch elapsed time, best elapsed time, median elapsed time, median absolute deviation, relative MAD, and best batch-amortized operations per second.
It emits no operation histogram and makes no latency-percentile claim.
Its fixed labels are `claim_scope: ci_regression_only` and `metric: batch_amortized_mean`.

For odd sample counts, the median is the middle sorted value.
For even sample counts, it is the overflow-safe integer midpoint of the two middle values.
MAD is the median of the absolute deviations from that integer median.
Relative MAD is MAD divided by median elapsed time.
Zero durations and non-finite JSON values are rejected.

The gate uses the minimum elapsed duration, equivalently maximum throughput, to reduce shared-runner interruption noise.
Min-of-N is optimistically biased and must not be interpreted as expected sustained throughput.
MAD remains visible to identify unstable jobs.
The checked-in 1,000,000 operations-per-second floor is intentionally far below the observed local 76,841,802 operations per second.
It is intended to catch gross regressions, accidental debug builds, or a broken hot path, not small performance changes.
The 25 percent relative-MAD cap is the first-line noisy-runner limit.
If public-runner history proves that cap flaky, widening it requires a documented follow-up change and retained dispersion reporting.

GitHub-hosted runners do not provide dependable PMU access.
The CI gate does not invoke `perf`, does not upload artifacts, and does not label any output publishable.
