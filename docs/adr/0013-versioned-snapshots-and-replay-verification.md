# ADR-0013: Versioned snapshots and deterministic replay verification

## Status

Accepted.

## Context

The command journal can recover accepted command order but replaying its complete lifetime becomes increasingly expensive.
Recovery must also reproduce generation-safe handles, free-list reuse, FIFO links, sequence exhaustion, and logical-time checks exactly.
Native object serialization would persist padding, pointers, derived caches, and implementation-specific layout.

## Decision

Use a versioned, fixed-width, little-endian snapshot format containing only canonical source state.
Preserve arena generations and free-list topology because they affect future observable handles.
Rebuild price levels, aggregates, and hierarchical occupancy from validated live order records.
Construct restore candidates transactionally and return ownership only after complete validation and the existing invariant checker succeed.

Persist snapshots with a temporary file, file synchronization, atomic rename through a retained parent-directory descriptor, and parent-directory synchronization.
Report a post-rename directory synchronization failure as indeterminate.

Keep the sequence-1 journal prefix for this phase.
Replay validates the snapshot boundary against that prefix, then applies only the suffix.
Use canonical event bytes and streaming CRC32C with explicit counts as a stable non-cryptographic fingerprint.
Continue to use exact event equality in correctness tests.

## Consequences

Snapshots recover physical arena behavior without tying the format to C++ object representation.
Restore performs bounded allocation and validation off the matching hot path.
Files are bounded to 1,000,000 slots and 48,000,112 bytes.
Crash recovery may need operator inspection after an indeterminate post-rename result.
CRC32C detects ordinary corruption but provides no authentication or collision resistance.
Journal compaction, rotation, suffix-only journals, replication, and cryptographic signing remain future work.
