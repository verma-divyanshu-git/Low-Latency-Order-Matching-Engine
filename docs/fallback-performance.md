# Indicative fallback performance

These measurements use the best currently available fallback host: a dedicated Apple M4 Pro running Darwin arm64.
They are retained because a dedicated local machine has less uncontrolled co-tenancy than free shared cloud runners.
The host is still not qualified under the project's Linux x86 latency contract.

## Batch throughput

Two independent runs each executed 21 repetitions of 1,000,000 crossing-limit operations.
The median batch-amortized throughput was 77.22 million and 75.33 million operations per second.
The two run medians differ by 2.48%.
Relative median absolute deviation was 2.01% and 1.21%.
Every repetition produced the same checksum.

This is batch-amortized matching throughput.
It is not a per-order latency measurement, sustained network rate, or end-to-end exchange throughput result.
The raw artifacts retain every elapsed batch duration and the exact checksum.

## Full-engine offered-rate tracking

Both crossing-limit and three-level sweep scenarios ran 100,000 measured events after 10,000 warmup events at requested rates of 100k, 250k, 500k, 1M, and 2M events per second.
Every run completed all 100,000 operations with zero invalid samples and achieved completion rate close to the requested rate.
Harness backlog was nonzero, so 2M is a tested offered rate rather than a measured saturation ceiling.

Every rate-sweep artifact is labeled `regression_only` and `operation_below_resolution`.
The latency buckets remain quantized diagnostics and are not published as precise latency.

## Reproduction

```sh
cmake --preset measurement
cmake --build --preset measurement
./build/measurement/order_book_throughput_gate \
  --samples 1000000 \
  --repetitions 21 \
  --min-ops-per-second 1 \
  --max-relative-mad 1
python3 scripts/run_rate_sweep.py \
  --executable ./build/measurement/order_book_benchmark \
  --scenario crossing-limit \
  --samples 100000 \
  --warmup 10000 \
  --output-dir benchmark-results/reproduction \
  100000 250000 500000 1000000 2000000
```

The [fallback manifest](../benchmark-results/phase8-fallback/manifest.json) records the exact revision, compiler, hardware, hashes, run medians, dispersion, checksum, and claim restrictions.
The [host report](../benchmark-results/phase7-host/mac-arm64.json) remains `qualified: false`.
