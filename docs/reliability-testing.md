# Reliability testing

Runtime reliability tests use deterministic public status schedules rather than production-only fault switches.
This exercises the same lifecycle and health transitions applications observe without adding a fault branch to the matching hot path.

The control-plane soak runs 100,000 progress cycles and injects ingress backpressure every 1,000 cycles, matching backpressure every 2,500 cycles, and journal capacity every 10,000 cycles.
It verifies exact accepted, matched, published, backpressure, capacity, failure, and graceful-shutdown counts.

A separate recovery schedule injects `commit_indeterminate` at an exact cycle and compares complete health snapshots across repeated runs.
Corruption injection verifies that internal invariant failure dominates prior transient health and stops ingress.

Persistence syscall tests provide lower-level deterministic injection for journal pre-publish and post-publish synchronization, snapshot write and rename, compaction unlink, and parent-directory synchronization.
Recovery tests cover partial, full, and empty-successor journal states, truncation rejection, and retry after interrupted compaction.

These tests model deterministic software-visible failure boundaries.
They do not claim to simulate defective storage firmware, power-loss ordering outside the filesystem contract, kernel defects, or hardware corruption beyond the documented checksum model.
