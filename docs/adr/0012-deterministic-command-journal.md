# ADR-0012: Deterministic command boundary and fixed-capacity journal

## Status

Accepted.

## Context

Deterministic in-memory replay is insufficient for restart recovery unless command bytes, ordering, logical time, and persistence boundaries are explicit.
Serializing native C++ objects would expose padding, endianness, enum representation, and pointer-layout differences.
Letting the matcher read clocks or files would also mix nondeterministic process concerns into its hot path.

## Decision

The gateway-facing command is one fixed tagged payload with canonical zero values for every unused field.
The only persisted representation is a specified 36-byte little-endian encoding.
A stateful sequencer assigns contiguous sequences starting at 1 and accepts nondecreasing caller-supplied logical time.

The sequenced engine wraps the existing order book.
It uses construction-time fixed scratch storage and caller-owned event output.
It validates and preflights output capacity before mutation.
Events include command sequence and per-command index, with one result followed by exact-order trade events.

Persistence is a separate `matching_engine::persistence` library.
The core library does not link it.
The journal is a fixed-capacity 64-byte header followed by 80-byte records.
Records publish a commit marker only after command bytes and CRC32C have been synchronized.
Recovery scans contiguous records and stops only at an exact zero marker while treating every other malformed marker or committed record as fatal corruption.

Applications must append successfully before applying the command.
The journal permits one writer in one process and thread and retains a nonblocking advisory exclusive file lock for its open lifetime.

## Consequences

The matcher remains independent of wall clocks, random generators, filesystems, floating point, dynamic output, and pointer-valued results.
Canonical bytes and golden vectors make cross-process replay representation explicit.
Fixed capacity and full-span synchronization make the first implementation intentionally simple but impose startup sizing and per-command persistence cost.

CRC32C detects accidental corruption and torn writes under the documented crash model.
It is not cryptographic integrity and cannot defend against an attacker who can rewrite the file.
`msync` and `fsync` durability still depends on filesystem and hardware behavior.
A failed synchronization after marker publication is explicitly indeterminate and poisons that writer until reopen recovery determines the record boundary.
No rollback write is used to claim that an uncertain commit is absent.

Mode `0600` including special-bit rejection, regular-file checks, exclusive creation, close-on-exec, no-follow opening, and advisory ownership locking reduce local file hazards.
They do not replace directory permissions or host access control.
Advisory locks exclude cooperating opens but cannot prevent access by software that ignores them.

There is no snapshot, compaction, rotation, replication, authentication, or automatic corruption repair in this phase.
Those capabilities must preserve this log's ordering and corruption policy if added.
