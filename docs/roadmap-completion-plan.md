# Completion roadmap

This is the tracked handoff for completing the project from the verified state on `main` as of 2026-08-26.
It supersedes stale branch and commit references in previous handoffs.

## Delivery rules

- Author commits as `Divyanshu <bautocrats@gmail.com>` and use only the `verma-divyanshu-git` GitHub account.
- Use one concern per pull request, merge only after every required check is green, then delete the merged feature branch.
- Keep the runtime core dependency-free, the hot path allocation-free and integer-only, and sequencing caller-clocked.
- Do not claim latency from CI, a macOS steady-clock fallback, batch-mean statistics, or a host that fails qualification.
- Preserve deterministic replay, trust-boundary validation, and reference-model coverage when extending exchange semantics.

## Verified on main

- The fixed-capacity matching core, order arena, generation handles, bounded price domain, bitmap, time-in-force behavior, cancel/amend/replace, invariants, reference model, differential tests, and fuzz harness exist.
- Measurement, host qualification, batch throughput gating, command encoding, caller-clocked sequencing, mmap journaling, snapshots, replay, and the three-stage SPSC pipeline exist.
- CI currently covers Ubuntu x86_64, macOS arm64, ASan, UBSan, TSan, release throughput regression, and a Clang fuzz smoke job.
- PR #7 corrected BBO encode status handling and passed every CI job before merge.
- The BBO protocol and publisher, command protocol, lane merge, gateway, market-data protocol, deterministic market-data file reader, and add-order market-data adapter exist.

## Remaining work

### Step 0: persist this roadmap

Create this document as a documentation-only pull request.
This is the current pull request.

### Phase 2: finish the concurrency boundary

Audit the existing lane merge, gateway, BBO, publication, and runtime-control components against the requirements below before adding behavior.

1. Add a lane identity to each producer path and prove deterministic merge ordering from logical time plus lane identity under arrival permutations.
2. Complete gateway validation for duplicate client order IDs, checked quantity and notional limits, price collars, and per-lane rate limits before sequencing.
3. Complete canonical publication state and bounded BBO reader retry behavior with TSan reader/writer coverage.
4. Add caller-supplied CPU affinity plus prefault and memory-lock result reporting without giving stages thread ownership.
5. Add shutdown, backpressure, starvation, wraparound, and recovery concurrency tests that compare threaded output with the single-thread reference.
6. Record the final decisions in ADR-0015 and ADR-0016.

Each numbered concern is its own pull request.
Phase 3 does not begin until this phase is complete.

### Phase 3: complete protocol ingestion and replay

The fixed BBO and market-data codecs, file reader, and add-order adapter are implemented.
The remaining work is:

1. Integrate market-data file replay into `replay_cli` and route all accepted input through the gateway before matching.
2. Map the supported market-data mutation messages with sequencing, gap, malformed, range, enum, and reserved-byte rejection before engine mutation.
3. Publish MBO and MBP views from the same canonical event stream.
4. Add decoder fuzz targets and malformed-input regression coverage for each public decoder.
5. Document a license-clean sample dataset, its URL, license, checksum, preprocessing command, and replay procedure.
6. Add protocol and data-set documentation plus ADR-0017, ADR-0018, and ADR-0019.

Do not describe a sample data set as a full trading day.

### Phase 4: exchange semantics

Extend the reference model first, then implement and differentially test each semantic independently.

1. Post-only handling.
2. Configurable self-trade prevention using trader identity, never order identity.
3. Iceberg replenishment.
4. Stop and stop-limit orders with a bounded trigger cascade.
5. Threshold pro-rata with FIFO residue.
6. Opening-cross auction behavior derived from a cited venue rule.

Every transition needs invariant, replay, snapshot, fuzz, and differential coverage.
Each semantic receives its own ADR with the cited venue rule.

### Phase 5: persistence operations

Add journal rotation with base-sequence semantics, snapshot-driven compaction, and deterministic recovery across rotated segments.
Add crash-point tests for rotation, rename, truncation, and compaction.
Document recovery operation and retain the CRC32C limitation as accidental-corruption detection only.

### Phase 6: operation boundaries

Add strict configuration parsing, startup and shutdown behavior, off-hot-path structured reporting, explicit health states, deterministic fault injection, packaging targets, API versioning compatibility tests, threat modeling, and resource-limit documentation.
Keep authentication, authorization, and real-money claims out of scope.

### Phase 7: measured optimization

Run only after a qualified host is available.
Establish qualified baselines, compare equivalent semantics against `std::map`, a sorted vector, and a reputable flat or B-tree implementation, then record accepted and rejected hypotheses with retained artifacts.
Do not remove validation to improve a benchmark.

### Phase 8: host qualification and artifacts

The current macOS host is regression-only.
Use a borrowed physical x86 Linux machine, university resource, or verified OSS bare-metal program for publishable measurement evidence.
Capture host qualification, topology, frequency policy, multiple repetitions, load sweeps, and all benchmark scopes.
A second independent run must reproduce artifacts before publication.

### Phase 9: evidence-driven README

Update the README only with code-generated diagrams and qualified artifacts.
Make regression-only results visually distinct.
Link each headline value to its manifest and reproduction command, and add an honest design-alternatives section.

### Phase 10: release gate

Require green CI and presets, deterministic snapshot-plus-journal replay, correct identity, a clean repository, complete artifacts, no paid or card-required infrastructure, no unsupported claims, and only `main` plus `develop` as retained integration branches.
Tag only after every condition holds.

## Execution order

Finish Step 0 first.
Then complete Phase 2 one concern per pull request.
Complete Phase 3 and Phase 4 in dependency order, while Phase 5 may proceed in parallel only after its branch and integration checks do not overlap those changes.
Phase 6 waits for Phases 2 through 5.
Phases 7 and 8 require a qualified host, then Phase 9 and the Phase 10 release gate follow.