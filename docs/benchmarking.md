# Order-book benchmarking

Phase 3B provides a benchmark harness, not a published performance claim.
Every run executes the measurement clock self-check before workload collection.
An unsafe clock fails the run.
A safe but non-publication-qualified clock still writes artifacts, prints the exact refusal reason, exits successfully, and labels the result `regression_only`.
The macOS steady-clock fallback is always regression-only.

## Build

```sh
cmake --preset measurement
cmake --build --preset measurement
ctest --preset measurement
```

`HdrHistogram_c` is fetched only when `ENGINE_BUILD_BENCHMARKS=ON`.
It is pinned to commit `18c7a324383dded1451d15621cd018b0048057d0`, corresponding to tag `0.11.10`.
The dependency links only to the benchmark support target.
The matching core has no runtime or public dependency on HdrHistogram.

## Open-loop method

Open-loop runs precompute every intended arrival offset before measurement.
The schedule uses checked integer rational arithmetic based on the requested events-per-second rate and the calibrated ticks-per-nanosecond ratio.
The event loop waits only while it is early.
A late event starts immediately.
Reported latency is completion tick minus intended arrival tick, so queueing delay is retained.
Scheduler lateness is actual start tick minus intended arrival tick.
Maximum event backlog and maximum lateness are reported independently.
Backward and migrated samples are discarded and counted.
Open-loop samples are recorded only with `hdr_record_value`.
Coordinated-omission correction is never applied to engine open-loop results.

All maker orders, the fixed-capacity book, trade output, schedules, and histograms are allocated before the measured loop.
Warmup executes after preload and before the measured schedule begins.
The timed operation is one aggressive IOC limit submission plus its matching work.
Result and trade-shape validation occurs immediately after each operation and contributes to a deterministic checksum.

`crossing-limit` gives every aggressive order one preloaded maker at price 101.
Each event produces exactly one trade.
`sweep-3-level` assigns each event a disjoint group of three ascending prices.
Each aggressive order consumes exactly one maker at each of those three levels in best-price order.
Disjoint groups are required because preloading multiple future makers at a shared best level would let an earlier taker consume more than one maker at that level.

```sh
./build/measurement/order_book_benchmark \
  --mode open-loop \
  --scenario crossing-limit \
  --samples 100000 \
  --warmup 10000 \
  --rate 100000 \
  --output-dir benchmark-results/crossing
```

Use `sweep-3-level` for the three-level workload.
The CLI accepts complete positive base-10 integers only and caps samples at 1,000,000.
It rejects unknown options, trailing text, parent traversal in output directories, incompatible diagnostic options, and capacities that cannot be represented by the book.

## Closed-loop coordinated-omission diagnostic

`closed-loop-diagnostic` is a separate synthetic demonstration.
It records one raw service-time histogram with `hdr_record_value` and one corrected histogram with `hdr_record_corrected_value`.
The expected interval is explicit.
The periodic stall is deterministic and explicitly synthetic.
Neither histogram is an order-book latency claim.
The files and JSON reasons include `raw`, `corrected`, and `diagnostic` labels so they cannot be confused with open-loop artifacts.

```sh
./build/measurement/order_book_benchmark \
  --mode closed-loop-diagnostic \
  --samples 10000 \
  --diagnostic-interval-ns 10000 \
  --diagnostic-stall-every 100 \
  --diagnostic-stall-ns 1000000 \
  --output-dir benchmark-results/diagnostic
```

The diagnostic rejects engine scenario, warmup, and rate options because they have no meaning for synthetic closed-loop values.

## Histograms and artifacts

Histograms use one-nanosecond lowest discernible value, a one-hour highest trackable value, and three significant decimal digits.
Every open-loop run writes a raw recorded-bucket CSV with `value,count`, a percentile distribution text file, and one strict JSON summary.
Recorded-bucket CSV rows come from HdrHistogram recorded-value iteration and retain exact bucket counts.
Diagnostic runs write separately named raw and corrected versions of all three artifacts.

JSON schema version 1 includes mode, scenario, count, min, p50, p90, p99, p99.9, p99.99, max, mean, requested and achieved rate, duration, maximum backlog, maximum event lateness, backward, migration, and total invalid counts, clock report, operation-resolution publication reason, checksum, and `claim_scope`.
The summary intentionally excludes hostname, username, and filesystem paths.
`publishable_candidate` means only that the clock source and 10x effective-granularity gate passed.
It is not publication approval and does not replace the environment disclosures required by ADR-0002.

After collection, the observed median service ticks are compared with ten times the self-check's effective granularity.
A source refusal or operation-resolution refusal produces `regression_only` artifacts and an exact reason.
No macOS steady fallback can become `publishable_candidate`.

## Explicit rate sweeps

The sweep script accepts rates chosen by the user and does not infer saturation or choose a rate automatically.
It validates every JSON input, writes a combined CSV, and creates a self-contained SVG with p50, p99, p99.9, and maximum backlog plots.
Regression-only points have a distinct hollow red style.

```sh
python3 scripts/run_rate_sweep.py \
  --executable ./build/measurement/order_book_benchmark \
  --scenario crossing-limit \
  --samples 10000 \
  --warmup 1000 \
  --output-dir benchmark-results/rate-sweep \
  25000 50000 100000 200000
```

Run sweeps on an otherwise controlled host and preserve every per-rate artifact.
Local results from fallback clocks are useful only for same-machine regression investigation.

## Primary sources

- Gil Tene, [How NOT to Measure Latency](https://www.infoq.com/presentations/latency-response-time/).
- Gil Tene, [HdrHistogram coordinated omission background](https://github.com/HdrHistogram/HdrHistogram#synopsis).
- [HdrHistogram_c](https://github.com/HdrHistogram/HdrHistogram_c), including `hdr_record_corrected_value` and recorded-value iteration.
