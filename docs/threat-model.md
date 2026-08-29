# Threat model

## Protected properties

The project protects deterministic command ordering, matcher state integrity, durable recovery boundaries, bounded resource use, and honest operational reporting.
Loss of availability is preferable to silently skipping, inventing, or reordering a command.

## Trust boundaries

Untrusted or fallible inputs cross these boundaries:

- Runtime configuration entries.
- Command and market-data protocol frames.
- Journal segments and snapshots loaded from storage.
- Filesystem paths and containing directories.
- Caller-provided queue scheduling, thread ownership, logical time, and output buffers.
- Host clocks and tuning data used for measurement claims.

## Addressed threats

Strict parsers reject malformed lengths, versions, enums, reserved bytes, duplicate or unknown config fields, noncanonical integers, overflow, unsafe resource relationships, and invalid paths before mutation or allocation where practical.
Fixed capacities bound books, queues, journals, snapshots, and event batches.
Checked integer arithmetic avoids floating-point and multiplication overflow at risk boundaries.

Persistence uses canonical encodings, CRC32C accidental-corruption detection, commit markers, synchronization, exclusive creation, `0600` mode checks, regular-file checks, no-follow behavior where supported, advisory writer locks, explicit base sequences, and gap and overlap rejection.
Recovery never skips committed corruption.
Snapshot replacement and compaction report post-rename or post-deletion directory-sync failures as indeterminate.

The matcher has one owning thread.
SPSC endpoints have one producer and one consumer.
Invalid sequence, logical-time regression, impossible capacity, internal invariant failure, persistence ambiguity, and corruption stop progress and require recovery.
Structured health reports contain fixed enum names and counters only.

## Operator responsibilities

Protect persistence directories against unauthorized creation, replacement, and deletion.
Preserve files before recovery.
Treat corruption and `commit_indeterminate` as stop conditions.
Authenticate and authorize any future network or administration layer outside this library.
Keep credentials, personal data, and customer identifiers out of config and operational reports.

## Not addressed

The project does not provide network transport, authentication, authorization, account permissions, encrypted storage, key management, cryptographic file authentication, replicated consensus, disaster-recovery replication, regulatory controls, or real-money trading safeguards.
CRC32C cannot detect deliberate modification by an attacker who can rewrite files and checksums.
Advisory locks do not stop software that ignores them.
The model does not defend against a malicious kernel, compiler, administrator, defective hardware that lies about persistence, or denial of service within explicitly configured capacity.
