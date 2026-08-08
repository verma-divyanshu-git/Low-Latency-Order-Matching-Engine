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
Replay validates the engine next sequence, logical time, and exhaustion state against the snapshot point before applying any suffix command.
Terminal snapshots remain loadable and preserve exhaustion, but replay rejects them as unverifiable with the current sequence-1 journal format.
Journal recovery rejects every nonzero commit marker after the first exact zero gap without parsing later payloads.
Bound both arena capacity and price levels at 1,000,000, with all price-level limits checked before allocation.
Use canonical event bytes and streaming CRC32C with explicit counts as a stable non-cryptographic fingerprint.
Continue to use exact event equality in correctness tests.

## Consequences

Snapshots recover physical arena behavior without tying the format to C++ object representation.
Restore performs bounded allocation and validation off the matching hot path.
Files are bounded to 1,000,000 slots and 48,000,112 bytes.
Price-domain allocations are bounded to 1,000,000 levels, roughly 48 MB for both level arrays on the supported ABI plus bounded occupancy summaries.
Together with the quantity bound, these limits prevent resource amplification and make aggregate overflow unreachable for a valid snapshot.
Crash recovery may need operator inspection after an indeterminate post-rename result.
CRC32C detects ordinary corruption but provides no authentication or collision resistance.
Journal compaction, rotation, suffix-only journals, replication, and cryptographic signing remain future work.
