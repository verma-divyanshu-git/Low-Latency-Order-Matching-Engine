# Persistence recovery runbook

Use this procedure after an unclean shutdown, a persistence error, or `commit_indeterminate`.
Do not edit journal or snapshot bytes to make recovery continue.

## Stop and preserve

1. Stop every writer and matcher process that uses the journal prefix.
2. Preserve the snapshot and all matching `<prefix>.<20-digit-base>.journal` files before changing anything.
3. Protect the containing directory.
   Journal and snapshot files require mode `0600`, but directory access still controls replacement and deletion.
4. Record the application revision, error, snapshot path, journal prefix, and filesystem involved.

## Validate and replay

For a rotated journal, run:

```sh
./build/production/matching_engine_replay \
  --journal-prefix /absolute/path/orders \
  --snapshot /absolute/path/engine.snapshot
```

For one legacy journal file, use `--journal PATH` instead of `--journal-prefix PREFIX`.
Exactly one journal input mode is required.

A successful replay emits one JSON line after complete replay and invariant validation.
A failure emits no partial-success JSON and returns nonzero.
Treat malformed names, header/name disagreement, overlap, gaps, committed corruption, a partial non-final segment, snapshot mismatch, and invariant failure as stop conditions.
Do not skip the reported segment or command.
Recover from an independently trusted copy or investigate the storage failure.

Use `--expect-crc32c`, `--expect-events`, and `--expect-live-orders` when independently recorded expected values are available.
The CRC32C value is only an accidental-corruption diagnostic.
It is not authentication and cannot prove that files were not deliberately modified.

## Resume writing

Application code resumes a validated rotated journal with `RotatingJournal::resume(prefix)`.
Resume validates the complete segment chain and adopts only the final segment.

- If the final segment is partial, the next append continues in that segment.
- If the final segment is full, the next append creates the exact base-named successor.
- If an empty successor already exists, the next append uses it without another rotation.
- A sequence or logical-time mismatch is rejected before rotation or append.

The application must append and receive `JournalError::none` before applying a command to the matcher.
After `commit_indeterminate`, close the writer and recover before deciding whether to retry the command.

## Snapshot replacement states

A failure before snapshot rename leaves the previous snapshot authoritative.
Temporary-file cleanup is attempted, but operators must inspect the directory if cleanup also fails.

A parent-directory synchronization failure after rename returns `commit_indeterminate`.
The replacement may be visible but its survival across another crash is unknown.
Stop, preserve the directory state, load and validate the visible snapshot, then replay the journal before resuming.

A truncated, oversized, checksum-invalid, or structurally invalid snapshot is rejected.
Do not pair an invalid snapshot with a compacted journal.
Use an older independently preserved snapshot whose required journal suffix is still complete.

## Compaction

Run compaction only with all journal writers closed and only after `save_snapshot_atomic` returns `SnapshotError::none` for the intended boundary.
`compact_journal_segments(prefix, snapshot_path)` validates both the snapshot and complete segment set before deletion.
It removes only whole segments covered by the snapshot.

If the snapshot covers every segment, compaction creates a durable empty successor at the next sequence before deleting covered files.
Compaction is retryable after interruption.
An unlink failure leaves a recoverable longer chain.
A parent-directory synchronization failure after deletion returns `commit_indeterminate`; preserve and validate the visible segment set before retrying.

Never delete the segment containing commands after the snapshot boundary.
Never compact from a snapshot ahead of journal history or older than the retained history.

## Return to service

Return to service only when all of these are true:

- The snapshot loads successfully.
- Segment discovery validates every retained segment.
- Replay reaches the end without a sequence gap, corruption, apply error, or invariant failure.
- Expected event and live-order values match when supplied.
- `RotatingJournal::resume` succeeds.
- The next command sequence and logical time are the expected values.

Keep the preserved pre-recovery files until the recovered engine has completed an independently verified checkpoint.
