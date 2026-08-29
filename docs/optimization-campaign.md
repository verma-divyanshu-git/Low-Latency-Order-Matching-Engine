# Measured optimization campaign

## Evidence scope

This campaign ran at source revision `36a024c19bc2b21cd65bea6b76d59875845539c1` with Apple Clang 21 on Darwin arm64, Apple M4 Pro.
The live host qualification report is `qualified: false` because the host is not Linux and cannot supply the required isolation, governor, clocksource, interrupt, PMU, and lock-limit evidence.
Every retained comparison and full-engine point is regression-only.
No latency value from this campaign is publishable.

## Predeclared hypotheses

- The dense ladder plus hierarchical bitmap should lead minimum-level lookup and mutation because it avoids tree traversal and per-node allocation.
- A B-tree may narrow the gap at larger active sets through cache-dense nodes.
- A sorted vector may compete at tiny active sets but should degrade when front erasure moves the active tail.
- `std::map` should provide stable ordered semantics but pay pointer traversal and allocation costs.
- Branch hints, huge pages, PGO, and BOLT should not be adopted without a repeatable full-engine improvement on a qualified host.

## Isolated price-index results

The workload repeatedly removes the minimum key and inserts the next key with identical quantities and checksum folding.
All four implementations produced the same checksum in every run.
Values below are median batch-amortized operations per second and are not operation latency.

| Active levels | Ladder + bitmap | `std::map` | Sorted vector | Abseil B-tree |
|---:|---:|---:|---:|---:|
| 64 | 117,589,371 | 22,614,411 | 49,591,884 | 62,363,580 |
| 4,096 | 106,894,709 | 26,894,069 | 1,029,393 | 48,886,823 |
| 65,536 | 108,059,511 | 25,030,506 | 50,751 | 28,924,029 |

The ladder leads every tested size.
The B-tree is consistently second.
Sorted-vector movement dominates beyond the tiny active set.
The result supports retaining the production price index and rejects replacing it with any tested alternative.

## Full-engine regression sweeps

Crossing-limit and sweep-3-level scenarios each ran 10,000 measured events after 1,000 warmup events at requested rates 25k, 50k, 100k, 200k, 500k, and 1M events per second.
Every run completed all operations with zero invalid samples and achieved completion rate close to the requested rate.
Every point is marked `regression_only` and `operation_below_resolution` with 41 ns effective granularity.
The retained percentile buckets therefore remain quantized diagnostics and are not precise latency measurements.

No production hot-path change was made.
The isolated comparison gives no evidence for replacing the ladder, while this host cannot qualify small full-engine latency changes.

## Rejected changes

- Replace ladder with `std::map`: rejected by isolated throughput.
- Replace ladder with sorted vector: rejected by scaling collapse as active levels increase.
- Replace ladder with Abseil B-tree: rejected because it remains slower and would add a production dependency.
- Add `[[likely]]` or `[[unlikely]]`: rejected without branch-counter evidence and qualified full-engine improvement.
- Enable transparent huge pages: rejected because host qualification requires them disabled for measurement stability and no engine evidence supports them.
- Add PGO or BOLT: deferred until a qualified Linux x86 host can produce reproducible full-engine evidence and retained profiles.

## Reproduction

```sh
cmake --preset measurement
cmake --build --preset measurement
./build/measurement/benchmark_comparison --active-levels 4096 --operations 100000 --repetitions 7
python3 scripts/run_rate_sweep.py --executable ./build/measurement/order_book_benchmark --scenario crossing-limit --samples 10000 --warmup 1000 --output-dir benchmark-results/reproduction 25000 50000 100000 200000 500000 1000000
scripts/verify-tuning.sh --benchmark-cpu 0 --sample-seconds 1 --output benchmark-results/host.json
```

The final verifier command is expected to return nonzero on macOS and shared hosts.
