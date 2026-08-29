# ADR-0024: Journal rotation and base sequences

## Status

Accepted.

## Decision

Journal format v2 stores a nonzero base sequence in header bytes 28 through 35.
A segment with base sequence $B$ requires record index $i$ to carry sequence $B + i$.
Creation rejects a base and capacity whose inclusive sequence range would overflow `uint64_t`.
Format v1 remains readable and has implicit base sequence 1.

`RotatingJournal` owns one active `MmapJournal` and uses deterministic segment names of the form `<prefix>.<20-digit-base>.journal`.
It validates payload, sequence, and logical time before rotation.
When the active segment is full, it closes that segment, creates the next segment at the next required sequence, and then appends the command.
A failure to close or create the next segment is returned before the command is applied.
Existing segment paths are never overwritten.

## Consequences

Segments have explicit ordering independent of directory iteration order or file timestamps.
A crash after closing a full segment but before creating its successor leaves a valid full prefix and no ambiguous command commit.
A crash after creating an empty successor leaves a valid empty segment at the exact next sequence.
Recovery must reject overlaps, gaps, malformed names, committed corruption, and non-final partial segments.

Rotation does not delete old segments.
Snapshot-driven compaction is a separate operation and may remove only segments proven obsolete by a durable snapshot boundary.
CRC32C remains accidental-corruption detection only and does not authenticate segments.
