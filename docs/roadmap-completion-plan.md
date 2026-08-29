# Completion roadmap

This document is the tracked handoff for completing the project.
Update the live status after every merged pull request and before handing work to another agent.

## Delivery rules

- Author commits as `Divyanshu <bautocrats@gmail.com>` and use only the `verma-divyanshu-git` GitHub account.
- Keep one active feature branch and one active pull request at a time.
- Start every new branch from the latest `main` after the preceding pull request is merged and deleted.
- Use one concern per pull request.
- Merge only after every required check is green.
- Never force-push without the owner requesting it.
- Delete merged feature branches locally and remotely.
- Keep the runtime core dependency-free, hot-path allocation-free, integer-only, and caller-clocked.
- Do not publish latency from CI, macOS steady-clock fallback, batch means, or an unqualified host.
- Preserve deterministic replay, trust-boundary validation, and reference-model coverage when adding exchange behavior.

## Live status

Last updated: 2026-08-28.

### Completed since this roadmap began

- `develop` was created from `main` and pushed to `origin`.
- Local Git hooks require `Divyanshu <bautocrats@gmail.com>` for commits and `verma-divyanshu-git` for pushes.
- The hooks reject identities outside the approved personal identity.
- PR #9 merged into `main` as `6b109fd`.
- PR #9 corrects market-data encoder error classification and adds focused regression coverage.
- Every PR #9 GitHub Actions check passed: Ubuntu x86_64, macOS arm64, ASan, UBSan, TSan, throughput regression, and Clang fuzz smoke.
- The focused `MarketDataProtocolTest` suite passed four of four tests after the PR branch was rebased.
- The local debug CTest catalog passed, including threaded pipeline, replay, and SPSC allocation checks.
- `tests/spsc_queue_test.cpp` contains the required `<thread>` include.
- Merged local feature branches were deleted.
- The obsolete `chore/stabilize-ci-and-identity` branch was reconciled and deleted.
  Its gateway implementation and build registrations were already in `main`, while its CI and CMake state was outdated.
- Phase 1 is complete.
  The `main` CI gate is green and `develop` is the only retained integration branch besides `main`.
- PR #13 added the lane-merge arrival-permutation oracle.
- PR #14 added bounded per-lane gateway rate limits and invalid-lane rejection.
- PR #15 added concurrent BBO reader coverage and exposed a torn-read defect on macOS.
- PR #16 fixed BBO seqlock ordering, and every CI job passed.
- PR #17 added lane-starvation coverage.
- PR #18 added ADR-0015 and ADR-0016.
- PR #19 routes market-data add orders through gateway validation before sequence assignment.
- Phase 2 is complete.
  Its lane, gateway, BBO, publication, runtime-control, threaded pipeline, and ADR requirements are implemented and covered.
- PR #21 added gateway-backed market-data replay through the matcher.
- PR #22 added generation-safe market-data delete mapping and replay lifecycle recording.
- PR #23 added generation-safe market-data replace mapping.
- PR #24 added canonical MBO event publication.
- PR #25 added public protocol decoder fuzz coverage.
- PR #26 fixed CI so every registered fuzz target is built before smoke testing.
- PR #29 added canonical MBP publication from authoritative post-event order-book state.
- Phase 3 is complete.
  Its replay, mutation mapping, MBO/MBP publication, decoder fuzzing, protocol documentation, dataset policy, and ADR requirements are implemented and covered.
- PR #39 merged threshold pro-rata and opening-cross into `main` at `1fbec37`.
- Phase 4 is complete.
  Its production and independent-reference semantics, deterministic allocation and uncrossing, invariants, command encoding, sequenced events, journal replay, snapshot compatibility, differential tests, fuzz coverage, protocols, and ADRs are implemented and covered.
- Post-merge verification passes all 10 focused Phase 4 core tests and all 5 focused persistence tests on `main`.

### Gates still open

- Keep this document and `plan-update-as-u-go-what-done-what-not.md` synchronized after each merged PR.

## Verified baseline on main

- The matching core, order arena, generation handles, bounded price domain, hierarchical bitmap, TIF, cancel, amend, replace, invariants, reference model, differential tests, and fuzz harness exist.
- Measurement, host qualification, throughput gating, command encoding, caller-clocked sequencing, mmap journaling, snapshots, replay, and the non-thread-owning three-stage SPSC pipeline exist.
- CI has Ubuntu x86_64, macOS arm64, ASan, UBSan, TSan, release throughput regression, and Clang fuzz-smoke jobs.
- Lane merge, gateway validation, BBO protocol and publication, runtime controls, command protocol, market-data protocol, market-data input, and the add-order market-data adapter exist.

## Ordered remaining work

### Phase 1: stabilize CI and branch hygiene

Completed on 2026-08-28.
The full local non-debug preset matrix remains useful repeatable evidence but is not a blocker after green `main` CI.

### Phase 2: audit and finish the concurrency boundary

Completed on 2026-08-28.
The listed requirements remain the acceptance record for the implementation and regression suite.
Use one pull request per numbered concern.

1. Prove merge order uses logical time plus lane identity and is invariant under arrival permutations.
2. Complete gateway checks for duplicate client order IDs, checked quantity and notional limits, price collars, and per-lane rate limits before sequencing.
3. Complete canonical publication and bounded BBO reader retry behavior with TSan reader and writer coverage.
4. Complete caller-supplied CPU affinity, prefault, and memory-lock result reporting without giving stages thread ownership.
5. Add shutdown, backpressure, starvation, wraparound, and recovery concurrency tests that compare threaded output with the single-thread reference.
6. Add ADR-0015 and ADR-0016.

### Phase 3: complete protocol ingestion and replay

Completed on 2026-08-29.
The listed requirements remain the acceptance record for the implementation and regression suite.

The accepted work is:

1. Route market-data replay through the gateway before matching.
2. Support accepted mutation messages with sequencing, gap, malformed, range, enum, and reserved-byte rejection before engine mutation.
3. Publish MBO and MBP views from the same canonical event stream.
4. Add decoder fuzz targets and malformed-input regression coverage for each public decoder.
5. Document a license-clean sample data set with URL, license, checksum, preprocessing command, and replay procedure.
6. Prefer a suitable license-clean Indian-market sample if one is available.
   Use the LOBSTER free sample only if it is the better documented license-clean choice.
7. Do not describe any sample data set as a full trading day.
8. Add protocol and dataset documentation plus ADR-0017, ADR-0018, and ADR-0019.

### Phase 4: exchange semantics

Extend the reference model first.
Implement and differentially test each semantic in a separate pull request.

1. Post-only handling.
2. Configurable self-trade prevention using trader identity, never order identity.
3. Iceberg replenishment.
4. Stop and stop-limit orders with a bounded trigger cascade.
5. Threshold pro-rata with FIFO residue.
6. Opening-cross auction behavior derived from a cited venue rule.

Completed on 2026-08-29 in PR #39.

Every transition needs invariant, replay, snapshot, fuzz, and differential coverage.
Each semantic needs an ADR with a cited venue rule.

### Phase 5: persistence operations

Add journal rotation with base-sequence semantics, snapshot-driven compaction, and deterministic recovery across rotated segments.
Add crash-point tests for rotation, rename, truncation, and compaction.
Document recovery and retain the CRC32C limitation as accidental-corruption detection only.

Journal format v2 base-sequence semantics and deterministic bounded rotation are implemented on `feat/journal-rotation`.
PR #40 merged that concern at `323fb1a`; all seven CI jobs passed.
Snapshot-driven whole-segment compaction, validated canonical segment discovery, and deterministic multi-segment replay are implemented on `feat/snapshot-journal-compaction`.
PR #41 merged that concern at `313b0df`; all seven CI jobs passed.
Resumable rotation, rotated-recovery CLI support, deterministic rotation and compaction crash tests, and `docs/recovery-runbook.md` are implemented on `feat/persistence-recovery-operations`.
Existing tests cover snapshot rename and journal and snapshot truncation crash boundaries.
The final branch passes 111 persistence tests, all 7 replay CLI tests, and the full 283-test debug catalog.
PR #42 merged the final recovery concern at `9a20576`; all seven CI jobs passed and the feature branch was deleted.
Post-merge verification passes all 5 focused recovery-window tests and all 7 replay CLI tests on `main`.
Phase 5 completed on 2026-08-29 in PRs #40, #41, and #42.

### Phase 6: operation boundaries

Add strict configuration parsing, startup and shutdown behavior, off-hot-path structured reporting, health states, deterministic fault injection, packaging targets, API-version compatibility tests, threat modeling, and resource-limit documentation.
Keep authentication, authorization, and real-money claims out of scope.

### Phase 7: measured optimization

Run only after host qualification is understood.
Establish qualified baselines and compare equivalent semantics against `std::map`, a sorted vector, and a reputable flat or B-tree implementation.
Record accepted and rejected hypotheses with retained artifacts.
Do not remove validation to improve a benchmark.

### Phase 8: free-host qualification and artifacts

Use the best free, no-card host available at the time.
The current macOS host and shared CI hosts are regression-only.
Do not present their latency as qualified.
Borrowed physical x86 Linux, a university resource, or a verified OSS bare-metal program is preferred for publishable evidence.
Capture qualification, topology, frequency policy, multiple repetitions, load sweeps, and all benchmark scopes.
Require an independent second run before publication.

### Phase 9: evidence-driven README

Update the README only with code-generated diagrams and qualified artifacts.
Make regression-only results visually distinct.
Link every headline value to its artifact manifest and reproduction command.
Add an honest design-alternatives section.

### Phase 10: release gate

Require green CI and presets, deterministic snapshot-plus-journal replay, correct identity, a clean repository, complete artifacts, no paid or card-required infrastructure, and no unsupported claims.
Retain only `main` and `develop` after all temporary branches are deleted.
Tag only after every condition holds.

## Execution order

Finish the outstanding Phase 1 gates first.
Then audit and finish Phase 2 one concern per pull request.
Complete Phase 3 and Phase 4 in dependency order.
Phase 5 may proceed only when its branch and integration checks do not overlap the active work.
Phase 6 waits for Phases 2 through 5.
Phases 7 and 8 need host evidence.
Phase 9 and the Phase 10 release gate follow.