# Snapshots and deterministic replay

Phase 4B adds explicit engine snapshots and the `matching_engine_replay` verifier.
Neither format serializes native C++ objects, padding, pointers, or floating-point values.

## Logical and physical state

A snapshot records the price-domain minimum and tick count, maximum order quantity, arena capacity and size, free-list head, each slot generation, liveness and free-list link, every live order field, sequence state, and the snapshot boundary.
Dead slot order payloads are canonical zero bytes, while their generations and free-list links remain physical state because they determine future handles.
Price levels, occupancy bitmaps, aggregate quantities, and invariant visit marks are derived state and are not serialized.
Restore builds a private candidate engine, validates the free list and live FIFO graph, rebuilds levels and occupancy, and publishes the engine only after the structural invariant checker succeeds.

The format is little-endian.
The header is 112 bytes and each slot record is 48 bytes.
At most 1,000,000 slots are accepted, making the maximum snapshot size 48,000,112 bytes.

The header layout is:

- Bytes 0-7: `MESNAP4\0`.
- Bytes 8-11: version 1.
- Bytes 12-15: header size 112.
- Bytes 16-19: slot size 48.
- Bytes 20-23: reserved zero.
- Bytes 24-31: exact total byte length.
- Bytes 32-39: signed minimum price ticks.
- Bytes 40-43: price-domain tick count.
- Bytes 44-47: maximum order quantity.
- Bytes 48-51: arena capacity.
- Bytes 52-55: live arena size.
- Bytes 56-59: free-list head.
- Bytes 60-63: sequence-exhausted flag, zero or one.
- Bytes 64-71: next sequence.
- Bytes 72-79: last logical time.
- Bytes 80-87: snapshot sequence.
- Bytes 88-95: snapshot logical time.
- Bytes 96-99: CRC32C.
- Bytes 100-111: reserved zero.

CRC32C covers every file byte except bytes 96-99 that contain the checksum itself.
CRC32C detects accidental corruption and is explicitly non-cryptographic.
It is not collision resistant, does not authenticate a file, and must not be used as a security decision.

Each slot stores generation at bytes 0-3, free-next at 4-7, liveness at 8, reserved zeros at 9-11, order ID at 12-19, remaining quantity at 20-27, previous and next indexes at 28-35, encoded level and side at 36-39, order reserved flags at 40-43, and reserved zeros at 44-47.
For a dead slot, bytes 12-47 are zero.

## Atomic persistence

Saving opens and retains the parent directory, creates a bounded unique `0600` temporary file with `openat`, writes and synchronizes the complete encoding, renames it over the destination with `renameat`, and synchronizes the retained parent directory.
The destination is never opened or followed before replacement.
A pre-rename failure leaves the previous destination in place and attempts to remove the temporary entry through the retained parent descriptor.
A parent-directory synchronization failure after rename returns `commit_indeterminate` because the replacement is visible but its survival after crash is unknown.

The process ID and a process-local atomic attempt counter form temporary names.
This only avoids ordinary collisions and makes no randomness or security claim.
The implementation retries at most 16 collisions.

Load rejects symlinks, non-regular files, files whose exact mode is not `0600`, truncation, oversized files, changed file identity or size during the read, malformed headers, noncanonical fields, CRC failure, and invalid engine graphs.
The containing directory must be protected because CRC32C does not defend against deliberate replacement.

`fsync` requests durability from the host filesystem and storage stack.
It cannot protect against defective devices that acknowledge uncompleted flushes, filesystem defects, or loss outside the host durability contract.

## Replay verifier

Without a snapshot, the CLI requires `--min`, `--max`, `--tick`, `--max-orders`, and `--max-quantity`.
Prices are already represented in integer ticks, so the current format requires `--tick 1`.
With a snapshot, its configuration is authoritative and all config flags are rejected.

The current journal format retains a prefix beginning at sequence 1.
For a nonzero snapshot boundary, replay requires the journal record at that exact sequence and requires its logical time to equal the snapshot logical time.
Records before the boundary are validated by journal recovery and skipped without applying them.
Every later command must equal the engine's next sequence, so gaps are never silently skipped.
Journal compaction, suffix-only journals, and rotation are not implemented yet.

Every replayed event is encoded into a canonical 64-byte little-endian record.
The streaming fingerprint reports CRC32C plus event and byte counts.
The fingerprint is a stable diagnostic, not a cryptographic commitment and not a substitute for exact event comparison.
Tests use exact ordered event equality as the correctness oracle.

The verifier reads no wall clock and emits one JSON line only after the full replay and invariant check succeed.
Expectation mismatches and all parse, persistence, apply, and invariant errors return nonzero without partial success JSON.
