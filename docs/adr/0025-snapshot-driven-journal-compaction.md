# ADR-0025: Snapshot-driven journal compaction

## Status

Accepted.

## Decision

Recovery discovers journal segments only through canonical `<prefix>.<20-digit-base>.journal` names.
It sorts them by parsed base sequence, opens and validates every segment, and rejects malformed matching names, header/name disagreement, overlaps, gaps, corruption, and any partial non-final segment before replay mutates the engine.

Replay indexes records relative to each segment's base sequence.
A durable snapshot at sequence $S$ may pair with a compacted first segment at $S+1$.
A first retained base greater than $S+1$ is a sequence gap.
When the snapshot boundary remains inside retained history, replay verifies its logical time before applying the suffix.

Compaction first loads and validates the durable snapshot and the complete closed segment set.
It deletes only whole segments whose end sequence is at or before the snapshot boundary.
If the snapshot covers every segment, compaction creates and synchronizes an empty successor at $S+1$ before deleting the covered prefix.
After deletion it synchronizes the parent directory.
It rejects a snapshot ahead of journal history and a snapshot older than the retained boundary without deleting files.

## Consequences

Recovery is independent of directory enumeration order and file timestamps.
Partial compaction can only leave a longer valid prefix or a valid retained suffix; it cannot authorize skipping a required command.
A segment containing commands after the snapshot is retained in full, so compaction is segment-granular rather than record-granular.

Compaction requires closed journal writers because segment validation takes the same exclusive advisory locks used by writers.
CRC32C detects accidental corruption only and does not authenticate snapshots or journal segments.
