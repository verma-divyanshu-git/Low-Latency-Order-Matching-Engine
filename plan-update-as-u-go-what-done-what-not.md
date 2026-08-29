see whats left now from the plan :

# Live execution ledger

Last updated: 2026-08-29.

## Current phase status

### Done

- Phase 1 is complete: branch, identity, CI, sanitizer, fuzz, measurement, and TSan gates are established.
- Phase 2 is complete: deterministic lane ingress, gateway validation, BBO publication, runtime controls, and concurrency regressions are merged.
- Phase 3 is complete: command and market-data protocols, gateway-backed replay, generation-safe mutation mapping, canonical MBO and MBP publication, decoder fuzzing, and protocol and dataset documentation are merged.
- Phase 4 post-only handling is merged.
- Phase 4 configurable trader-based self-trade prevention is merged.
- Phase 4 iceberg replenishment is merged in PR #37 at `c1025d0`.
- Phase 4 stop and stop-limit orders are merged in PR #38 at `382bb0d`.
- Phase 4 threshold pro-rata and opening-cross auction are merged in PR #39 at `1fbec37`.
- Phase 4 is complete: every advanced semantic has production, independent-reference, differential, invariant, replay, snapshot, fuzz, protocol, and ADR coverage.
- Phase 5 journal rotation, base-sequence semantics, multi-segment recovery, snapshot-driven compaction, crash recovery, and operator guidance are merged.
- Phase 5 is complete in PRs #40, #41, and #42; final recovery operations merged at `9a20576`.

### In progress

- Phase 6 strict runtime configuration and API compatibility are implemented on `feat/runtime-config`.
- PR #44 merged strict runtime configuration and API compatibility at `b16d4b2`.
- Runtime lifecycle, graceful shutdown, health-state escalation, bounded counters, and off-hot-path structured reporting are implemented on `feat/runtime-operations`.
- PR #45 merged runtime lifecycle, health, graceful shutdown, and structured reporting at `b03c337`.
- Deterministic status fault schedules and a 100,000-cycle control-plane soak are implemented on `test/runtime-reliability`.
- PR #46 merged deterministic fault schedules and soak coverage at `a7f9068`.
- CMake install/export/package targets, external consumer compatibility tests, threat modeling, resource limits, and corrected scope documentation are implemented on `feat/package-security-boundaries`.
- Phase 6 implementation is complete locally: strict config, lifecycle, graceful drain, structured reporting, health states, deterministic fault injection, soak coverage, API compatibility, install/export/package targets, threat model, and resource-limit documentation are present.
- The final Phase 6 branch passes 297 of 297 debug tests, including installed-package consumer compilation, and builds binary and source TGZ packages.

### Not started

- Phase 6 needs final package/security PR integration, green CI, and branch deletion.
- Phases 7, 8, 9, and 10 are not started.

## Detailed execution history

- Recovery checkpoint: terminal GPU acceleration is disabled and the restarted VS Code window is stable.
- Done: PR #37 merged Phase 4 iceberg replenishment into `main` at `c1025d0`; the feature branch was deleted.
- Fixed: PR #37's matrix failures came from the reference model not clamping displayed quantity after an amend. A minimized fuzz input then exposed iceberg validation precedence; both fixes are merged.
- Verified: the final local debug catalog passed 231 of 231 tests and both 10,000-execution fuzz smoke tests passed. Post-merge CI is running.
- Done: PR #38 merged Phase 4 stop and stop-limit orders into `main` at `382bb0d`; the feature branch was deleted.
- Fixed: PR #38's first Clang fuzz run exposed missing dormant-stop amend semantics in the reference model; the minimized input now passes and direct regressions cover zero, increase, and decrease cases.
- Verified: the stop-order branch passes the full local debug catalog, 243 of 243 tests, and both 10,000-execution fuzz smoke tests.
- In progress: `feat/threshold-pro-rata` contains an applied, uncommitted threshold pro-rata implementation across the production book, independent reference model, differential simulator, snapshot v5, and focused tests.
- Fixed: snapshot format v5 made the unsupported-version regression's old version byte valid; the test now mutates v5 to version 6.
- Verified: the pro-rata branch passes the full local debug catalog, 250 of 250 tests, and both 10,000-execution fuzz smoke tests.
- Implemented: opening-cross accumulation, deterministic uncross price selection, FIFO execution, continuous-state transition, command and event encoding, replay, snapshot v6, independent reference behavior, differential checks, fuzz coverage, and ADR-0023.
- Fixed: opening-auction fuzzing exposed a rejection-precedence mismatch in the independent reference model; the minimized input now passes.
- Verified: the combined Phase 4 working tree passes 260 of 260 debug tests and both 10,000-execution fuzz smoke tests.
- Done: PR #39 merged threshold pro-rata and opening-cross into `main` at `1fbec37`; `main` matches `origin/main` and the feature branch is deleted.
- Verified after merge: all 10 focused Phase 4 core tests and all 5 focused snapshot, event-codec, and replay tests pass on `main`.
- Next: implement Phase 5 persistence operations one concern at a time.
- Implemented: journal v2 stores an explicit base sequence, preserves v1 compatibility, rejects sequence-range overflow, and rotates to deterministic base-named segments without overwriting existing files.
- Verified: all 5 focused journal v2 and rotation regressions pass.
- Done: PR #40 merged journal rotation and base-sequence semantics into `main` at `323fb1a`; all seven CI jobs passed and the feature branch was deleted.
- Implemented: canonical segment discovery rejects malformed names, overlap, gaps, corruption, and partial non-final segments before replay.
- Implemented: replay accepts a compacted first segment only at `snapshot_sequence + 1`, traverses contiguous rotated segments, and rejects missing suffix commands.
- Implemented: snapshot-driven compaction deletes only covered whole segments and creates a durable empty successor when the snapshot covers the full journal.
- Verified: all 11 focused segment discovery, compacted replay, and compaction tests pass.
- Done: PR #41 merged snapshot-driven compaction and multi-segment replay into `main` at `313b0df`; all seven CI jobs passed and the feature branch was deleted.
- Implemented: `RotatingJournal::resume` validates the complete chain and resumes a partial, full, or already-created empty final segment without sequence or logical-time ambiguity.
- Implemented: `matching_engine_replay --journal-prefix` validates and replays rotated segment sets; it is mutually exclusive with legacy `--journal` mode.
- Added: deterministic crash-window coverage for rotation before and after successor creation and compaction unlink and parent-fsync failures.
- Existing coverage verifies snapshot pre-rename preservation, post-rename indeterminate durability, journal and snapshot truncation rejection, and post-publish commit recovery.
- Added: `docs/recovery-runbook.md` documents stop, preserve, validate, replay, resume, compaction, and return-to-service procedures and states explicitly that CRC32C detects accidental corruption only.
- Verified: the final Phase 5 branch passes 111 persistence tests, all 7 replay CLI tests, and 283 of 283 debug tests.
- Done: PR #42 merged resumable rotation, rotated-recovery CLI support, deterministic crash-window tests, and the recovery runbook into `main` at `9a20576`; all seven CI jobs passed and the feature branch was deleted.
- Verified after merge: all 5 focused rotation and compaction recovery-window tests and all 7 replay CLI tests pass on `main`.
- Phase 5 is complete.
- Next: begin Phase 6 production operation boundaries one concern at a time.
- Done: PR #44 merged strict runtime configuration and API compatibility into `main` at `b16d4b2`; all seven CI jobs passed.
- Done: PR #45 merged runtime lifecycle, graceful drain, health states, saturating counters, and fixed-schema reporting into `main` at `b03c337`; all seven CI jobs passed.
- Done: PR #46 merged deterministic status fault schedules and 100,000-cycle soak coverage into `main` at `a7f9068`; all seven CI jobs passed.
- Implemented: standard CMake install and exported targets, package config and version files, replay executable installation, CPack binary and source TGZ generation, and an external installed-package consumer test.
- Documented: runtime configuration, operations, reliability testing, packaging, threat model, resource limits, and explicit exclusions for authentication, authorization, cryptographic integrity, replication, regulation, and real-money use.
- Verified: the final Phase 6 branch passes 297 of 297 debug tests and generates both binary and source packages.

- Done: `develop` now exists on `origin` from current `main`.
- Done: local identity hooks require Divyanshu's personal commit identity and the approved GitHub account.
- Done: PR #9 merged the market-data encoder error-classification fix into `main`.
- Done: every PR #9 GitHub Actions job passed: Ubuntu x86_64, macOS arm64, ASan, UBSan, TSan, throughput regression, and Clang fuzz smoke.
- Done: the focused market-data protocol suite passed 4 of 4 tests after rebasing the PR branch, and the local debug CTest catalog passed.
- Done: `tests/spsc_queue_test.cpp` has the required `<thread>` include.
- Done: merged local feature branches were removed.
- Done: reconciled and removed the old `chore/stabilize-ci-and-identity` branch because its gateway implementation and build registrations were already in `main`; its CI/CMake state was obsolete.
- Done: PR #10 merged the tracked `docs/roadmap-completion-plan.md` into `main`.
- Done: PR #11 merged the current Phase 1 status updates into the tracked roadmap.
- Done: every PR #11 GitHub Actions job passed: Ubuntu x86_64, macOS arm64, ASan, UBSan, TSan, throughput regression, and Clang fuzz smoke.
- Done: Phase 1 is complete. Green `main` CI is accepted as the phase gate; the full local non-debug matrix remains repeatable evidence, not a blocker.
- Done: PR #12 merged Phase 1 completion into the tracked roadmap.
- Done: PR #13 merged the lane-merge arrival-permutation oracle into `main`.
- Done: PR #14 merged bounded per-lane gateway rate limits and invalid-lane validation into `main`.
- Done: PR #15 added the concurrent BBO reader regression; macOS exposed a torn-read defect.
- Done: PR #16 fixed the BBO seqlock ordering defect; every CI job passed.
- Done: PR #17 merged the lane-starvation regression into `main`; every CI job passed.
- Done: PR #18 merged ADR-0015 and ADR-0016 for deterministic lane ingress, gateway validation, bounded BBO snapshots, and caller-owned runtime controls.
- Done: PR #19 merged gateway validation before market-data sequence assignment; every CI job passed.
- Done: Phase 2 is complete.
- Done: PR #20 merged Phase 2 completion into the tracked roadmap.
- Done: PR #21 merged gateway-backed market-data replay through the matcher; every CI job passed.
- Done: PR #22 merged generation-safe market-data delete mapping and replay lifecycle recording; every CI job passed.
- Done: PR #23 merged generation-safe market-data replace mapping; every CI job passed.
- Done: PR #24 merged canonical MBO event publication.
- Done: PR #25 merged public protocol decoder fuzz coverage; its first fuzz CI run exposed that only the older fuzz target was built.
- Done: PR #26 merged the CI target-list fix for decoder fuzz; x86_64, ASan, UBSan, TSan, and Clang fuzz smoke are still running after merge.
- Next: Phase 3, confirm the decoder fuzz CI repair, then document the protocol and license-clean replay dataset before taking MBP publication.

The tracked version of this execution state is `docs/roadmap-completion-plan.md`.

Plan: complete the "What is left" roadmap
A companion to handoff.md that another agent could execute. Implementation step 0 is to persist this as a tracked docs/roadmap-completion-plan.md, since you want a durable handoff doc. Phases are dependency-ordered for speed and reliability; two tracks run in parallel.

Verified baseline (don't redo): matching core, arena + generation handles, price domain, hierarchical bitmap, TIF/cancel/amend/replace, invariant checker, reference book + differential sim + fuzzer, measurement/clock/benchmark/gate, command encoding + sequenced engine + mmap journal, snapshot + replay, SPSC queue + 3 non-thread-owning stages, 14 ADRs. All confirmed in code.

Git reality: only main exists (no develop, no feature branches); local main (de083fa) is 1 ahead of origin (8ecf42e); the ci.yml (ubuntu-24.04) and ProjectOptions.cmake (std::expected probe) fixes are already in the working tree uncommitted; custom identity hooks present.

Sequencing

P1 gates everything.
Track A: P2 → {P3, P4}. Track B: P5 runs in parallel from the start (independent of P2 to P4).
Both tracks converge before P6. Then P7 and P8 (intertwined, need a host), then P9, then P10.
Phase 1 - Stabilize branch, identity, CI (blocker)

Retarget the local pre-commit/pre-push hooks to author Divyanshu <bautocrats@gmail.com> without weakening the Cisco-identity rejection.
Pre-flight identity (gh api user, git var ...IDENT, git remote -v).
Create develop; adopt one-concern PR into named feature branch, merge to main, delete branch, end state only main + develop.
Re-verify the <thread> include in spsc_queue_test.cpp.
Fresh-configure and run the full matrix reading complete output: debug, release, asan, ubsan, tsan, fuzz, measurement.
Confirm the std::expected probe in ProjectOptions.cmake fails on a stdlib lacking it.
Commit the CI/toolchain fix separately as Divyanshu, PR into main, push only after green + identity checks.
Confirm all Actions jobs green; replace stale hashes in progress.md with stable phase text.
Decision: add a tsan CI job (only local today). Recommend yes.
Phase 2 - Concurrency: lanes, gateway, merge, BBO, publication, affinity

One SpscQueue lane per producer (reuse existing queue, add LaneId).
Deterministic lane merge independent of thread timing (caller logical time + LaneId tie-break); merge-oracle test permutes arrival timing, asserts identical ordering; sequencer still never reads a clock.
Gateway before sequencing: duplicate order-ID rejection (the client-ID map lives here, not the core), max qty/notional, price collar, per-lane rate. Notional uses checked wide integer, never float.
Seqlock BBO with a documented bounded reader-retry, plus a TSAN reader/writer test.
Canonical publication stage feeding downstream (foundation for MBO/MBP).
Caller-supplied CPU affinity; prefault + mlock with explicit failure reporting; stages stay non-thread-owning.
TSAN suite for shutdown, backpressure (must not mutate engine), lane starvation, wraparound, recovery; threaded output byte-identical to the single-thread reference.
New: gateway.hpp/.cpp, lane_merge.hpp, bbo.hpp, affinity.hpp; modify pipeline.hpp, command.hpp. ADR-0015/0016.
Phase 3 - Binary protocol + free real-data replay (depends on P2)

OUCH-inspired command protocol: fixed layout, explicit little-endian, versioned, static offset asserts, reject malformed length/enum/reserved/gap/range before engine mutation.
ITCH-inspired market-data protocol with sequence numbers + gap detection.
Golden-byte, round-trip, malformed, and fuzz targets per decoder.
Free, license-clean dataset (LOBSTER free sample is the safe default); record URL, license, checksum, exact preprocessing; never call a tiny sample a full trading day.
Replay through gateway to matcher; derive MBO and MBP from the same event stream; snapshot recovery + replay verification.
New: wire/command_protocol.*, wire/market_data_protocol.*, md/mbo_publisher.*, md/mbp_publisher.*, itch_replay_cli.cpp, docs/protocols.md, docs/datasets.md. ADR-0017/0018/0019.
Phase 4 - Advanced exchange semantics (partly parallel with P3)
Extend the reference model first, then add differential + invariant + replay + snapshot + fuzz per transition: post-only, configurable self-trade prevention, iceberg replenishment, stop/stop-limit with bounded trigger cascade, CME-style threshold pro-rata with FIFO residue, opening-cross auction from a cited spec. One ADR per semantic citing the venue rule.

Phase 5 - Persistence operations (independent, parallel from start)
Journal rotation with base-sequence semantics; snapshot-driven compaction; deterministic recovery across rotated segments (never skip committed corruption or gaps); crash-point tests around rotation/rename/truncation/compaction; recovery runbook; CRC32C stays "accidental corruption only." Modify journal.cpp, snapshot.cpp, replay.cpp; add docs/recovery-runbook.md.

Phase 6 - Production operation boundaries (after P2 to P5)
Strict config parse/validate; clean startup/shutdown; off-hot-path structured reporting (no secrets/PII); health states (corruption/capacity/backpressure/recovery-required); soak + deterministic fault injection; CMake install()/package targets; API versioning + compat tests; threat-model + resource-limit docs. Keep authN/authZ/real-money out of claims.

Phase 7 - Measured optimization campaign (needs P8 host)
Qualified baselines first; ladder+bitmap vs std::map, sorted vector, and one reputable flat/B-tree under identical semantics; isolate branch/cache/allocation hypotheses; PGO/BOLT/huge-pages/[[likely]] only with evidence; publish rejected hypotheses; never remove trust-boundary validation. Log every result.

Phase 8 - Qualify a free host + capture numbers
Free-host research result: there is no genuinely free, no-card, qualifiable bare-metal host on the open market.

Oracle Always Free requires a card at signup (confirmed in Oracle's FAQ). AWS/GCP/Azure free tiers need a card. Intel's classic DevCloud is retired.
No-card but shared/virtualized (GitHub Actions, Colab, Kaggle, Codespaces) cannot satisfy isolcpus/SMT/governor/invariant-TSC qualification, so per the handoff they are correctness/throughput-regression only, never a published sub-microsecond latency claim.
Your Mac is arm64; its steady-clock fallback is permanently regression-only.
Realistic routes to a true qualified number, in order: (1) borrow a physical x86 Linux box for one day (a friend's idle desktop or gaming PC with an Intel/AMD invariant TSC, or a university lab machine), boot Linux with isolcpus + performance governor, which qualifies far better than any cloud VM and costs nothing; (2) GitHub Student pack or university HPC if you have an .edu; (3) community bare-metal-for-OSS programs only after verifying current no-card terms. Then run the host verifier, pin threads, disclose CPU/SMT/NUMA/frequency/virtualization, multiple reps, sweep past the knee, capture all five scopes, and have a second agent reproduce before publishing.

Phase 9 - Evidence-driven README
Keep honest limitations; add code-generated technical SVGs and qualified plots from retained artifacts (regression-only points visually distinct, unresolved points labeled); link every headline number to a manifest + reproduction command so README/release/resume agree exactly; add a design-alternatives section; resume bullets only after qualified values exist.

Phase 10 - Final release gate
Green CI everywhere; all presets pass; deterministic replay from snapshot + journal; clean repo + correct identity; complete artifact manifest; no paid/card infra; no unsupported claims; only main + develop remain; tag the release.

Cross-cutting rules: author Divyanshu <bautocrats@gmail.com> / GitHub auth only verma-divyanshu-git / never Cisco / no bot co-author / no force-push without owner request; one concern per PR, delete merged branches; runtime core stays dependency-free and the hot path allocation-free, integer-only, no wall-clock; never publish CI latency, batch-mean-as-median, sub-clock ns, or 1/avg throughput.

**Open decisions for you**

1. Phase 8 host: try to borrow one-day x86 bare metal for a real qualified number, or accept indicative-only numbers and scope resume claims to throughput + correctness? Recommend attempting the bare-metal loan.
2. Add a `tsan` CI job in Phase 1? Recommend yes.
3. Phase 3 dataset: confirm the free sample. Recommend the LOBSTER free sample as the default.

1. for now use whichever can produce best numbers for phase 8 [when it comes to phase 8]. be it github actions or whatever. free & no cc. or mac even. then if not able to do satisfactory; we'll see later then.
2. cool add
3. idk what LOBSTER free sample is. but if its not indian market : i'd prefer similar indian market data? NSE/BSE/MCX i dont care. but if not available and LOBSTER is best then cool go w that. no need to confirm w me again. if LOBSTER best for this project, use it no worries.

lets complete one phase at a time. create its PR, merge it to main, delete not needed branch. one at a time.  pls keep pushing small commits and PRs (one thing per PR; can have multiple commits in one PR tho)..and keep merging also as and when required PRs; merging PRs and deleting unneeded branches. pls continue and complete all 10 phases now. dont let me repeat all this again.
these rules MUST not break. main is the branch where its all good rn ... ensure u only open one feature branch at a time : w one change (can be a feature or part of a feature if feature is lil big) : then create a PR to main; no need fancy PR desc or anything. commit msgs can be one liners simple no buzzwords. merge it . delete the feature branch . then work on next feature branch stemmed from latest main (after all merged and done)

lets go. plan-update-as-u-go-what-done-what-not so new agents can keep track.

such commands that basically print logged in user etc in a new screen (like the less command opens a new screen to show).. dont run them. they hang the system and mess up. AND
Run one terminal command at a time and wait for it to finish.
Keep command output tightly capped.
Avoid broad log searches, verbose Git graphs/diffs, and combined command chains.
Avoid interactive or long-running gh commands in the terminal.
Use direct file, test, diagnostics, CMake, and GitHub tools when available.
 i've already ran these commands so u dont have to ensure this on every commit/push i believe:
```
╭─    ~/Low-Latency-Order-Matching-Engine    feat/market-data-protocol *1 !3 ?1 
╰─ git config --local user.name "Divyanshu" && git config --local user.email "bautocrats@gmail.com"

╭─    ~/Low-Latency-Order-Matching-Engine    feat/market-data-protocol *1 !3 ?1 
╰─ gh auth switch --hostname github.com --user verma-divyanshu-git && gh auth setup-git   ─╯
✓ Switched active account for github.com to verma-divyanshu-git

╭─    ~/Low-Latency-Order-Matching-Engine    feat/market-data-protocol *1 !3 ?1 
╰─                                                                                        ─╯
```