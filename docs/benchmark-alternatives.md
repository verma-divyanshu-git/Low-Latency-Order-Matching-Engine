# Price-level index comparison

`benchmark_comparison` isolates one ordered price-level operation shared by four containers:

1. Start with identical unique integer keys and deterministic quantities.
2. Read and remove the current minimum key.
3. Insert the next monotonically increasing key and quantity.
4. Fold every removed key and quantity into the same checksum.

Implementations are the production hierarchical bitmap with a dense quantity array, `std::map`, a reserved sorted vector, and Abseil `btree_map`.
Abseil is fetched only when benchmarks are enabled and is pinned to release `20240116.2`.
It is not linked into the matching core or persistence libraries.

Every repetition constructs and preloads fresh state before timing.
The timed interval contains only repeated minimum removal, next-key insertion, and checksum folding.
The report requires identical checksums across all implementations.
It records every elapsed batch duration, median batch duration, and median batch-amortized operations per second.

The report always uses `claim_scope: regression_only` and `metric: batch_amortized_mean`.
It contains no latency percentiles.
This microbenchmark isolates price-level index behavior and does not represent complete order matching, queueing, persistence, protocol parsing, or market-data publication.
Results are comparable only within the same executable, revision, compiler, host, active-level count, operation count, and repetition count.

Example:

```sh
./build/measurement/benchmark_comparison \
  --active-levels 4096 \
  --operations 100000 \
  --repetitions 7 \
  --output benchmark-results/comparison.json
```
