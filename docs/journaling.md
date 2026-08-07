# Command sequencing and journal format

Phase 4A adds deterministic command ingress, deterministic engine events, and an optional fixed-capacity memory-mapped command journal.
Snapshots and compaction are deliberately absent.

## Matcher boundary

`CommandPayload` is a fixed trivially-copyable value, but its native object representation is never persisted.
Factories canonicalize unused fields to zero.
Validation rejects unknown command tags, invalid side or time-in-force values, a nonzero reserved byte, and nonzero unused fields.
The canonical command encoding is exactly 36 bytes in little-endian order:

- Offset 0, 1 byte: command tag.
- Offset 1, 1 byte: side.
- Offset 2, 1 byte: time in force.
- Offset 3, 1 byte: reserved zero.
- Offset 4, 8 bytes: order identifier.
- Offset 12, 8 bytes: signed price ticks in two's-complement representation.
- Offset 20, 8 bytes: quantity.
- Offset 28, 4 bytes: handle index.
- Offset 32, 4 bytes: handle generation.

The sequencer starts at sequence 1 and accepts caller-supplied logical time.
It never reads a clock.
It rejects decreasing logical time, invalid payloads, and sequence exhaustion without advancing its state.

`SequencedEngine::apply` validates the command and preflights caller event capacity before mutating the book.
Submission and replacement callers provide one result slot plus the book's maximum trade count.
Cancel and amend callers provide one result slot.
The engine owns its fixed trade scratch storage from construction, so `apply` performs no allocation.
Each stream starts with a result event and then contains trades in exact matching order, with command sequence and zero-based or increasing per-command event indexes.

The matching core does not read wall time, use random numbers, access files, use floating point, or return pointer-valued results.

## Journal file format

All integers are little-endian.
Native structures, padding, and pointers are never serialized.

The file header is 64 bytes:

- Offset 0, 8 bytes: magic `MEJNL4A\0`.
- Offset 8, 4 bytes: format version, currently 1.
- Offset 12, 4 bytes: header size, 64.
- Offset 16, 4 bytes: record size, 80.
- Offset 20, 8 bytes: fixed record capacity.
- Offset 28, 36 bytes: reserved zeros.

Each record is 80 bytes:

- Offset 0, 4 bytes: commit marker.
- Offset 4, 8 bytes: sequence.
- Offset 12, 8 bytes: logical time.
- Offset 20, 4 bytes: payload length, 36.
- Offset 24, 36 bytes: canonical command payload.
- Offset 60, 4 bytes: CRC32C over bytes 4 through 59.
- Offset 64, 16 bytes: reserved zeros.

The committed marker has little-endian numeric value `0x54494d43`.
Zero and every other marker value are treated as uncommitted or torn.
CRC32C uses the Castagnoli polynomial and is integrity and accidental-error detection only.
It is not a cryptographic authenticator and does not protect against deliberate modification.

Capacity is fixed at creation from 1 through 1,000,000 records.
The maximum file size is 80,000,064 bytes.
Creation uses exclusive creation, mode `0600`, close-on-exec, and no-follow behavior where the platform supplies it.
Only regular files with exact mode `0600`, exact size, and a valid canonical header are accepted.
The implementation supports macOS and Linux.

## Append, recovery, and durability

The required application order is append command, receive a successful commit result, then apply the command to the matcher.
Applying before append can produce state that recovery cannot reconstruct.

Append writes an uncommitted record, synchronizes its payload and CRC, publishes the commit marker last, then synchronizes again.
It checks mapping, synchronization, file synchronization, truncation, open, and explicit close failures.
A full journal and validation failures do not mutate append state.

Recovery scans from slot zero and requires sequences `1, 2, ...`, nondecreasing logical time, valid CRC, canonical payload bytes, and zero reserved bytes.
It stops at the first uncommitted or torn marker.
A malformed committed record is corruption and recovery stops with an error.
Recovery never skips a committed corrupt record and does not trust a header count.

`msync(MS_SYNC)` plus `fsync` asks the operating system and filesystem to persist each stage.
It cannot guarantee survival against defective hardware, storage devices that lie about flush completion, filesystem defects, or loss outside the documented host durability contract.
The protocol detects incomplete records under the assumed ordered flush behavior, but it is not a replicated consensus log.

The journal has one writer owned by one process and one thread.
There is no interprocess lock or concurrent append support.
Indexed reads return decoded command values, not views into the mapping.

## Operational policy

Treat journal corruption as a stop condition requiring operator investigation and recovery from an independently trusted source.
Do not rewrite, skip, or guess past corruption.
Protect the containing directory as well as the `0600` file.
Capacity exhaustion is explicit and requires planned rotation in a future snapshot and compaction phase.
