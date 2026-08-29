# ADR-0027: Runtime lifecycle and health reporting

## Status

Accepted.

## Decision

Operational lifecycle is a non-thread-owning state machine driven by existing pipeline statuses.
Startup, ingress acceptance, graceful draining, stopped state, and terminal failure are explicit.
Shutdown rejects new ingress first and reaches stopped only after command and event queues are both empty.

Health severity is monotonic for one runtime instance and distinguishes healthy, backpressured, capacity-exhausted, recovery-required, and corruption states.
Persistence ambiguity and corruption stop ingress.
Counters saturate rather than wrap.

The hot path updates and copies a fixed `RuntimeHealthSnapshot` without allocation or I/O.
A separate off-hot-path function serializes fixed-schema JSON.
Reports contain only enum names and counters, never arbitrary text, paths, order identifiers, secrets, credentials, or personal data.

## Consequences

Applications retain thread, scheduling, signal, and watchdog ownership while sharing one deterministic operational contract.
Backpressure remains observable without becoming a fatal error.
Recovery and corruption remain sticky until a new validated runtime instance starts.
The JSON report is operational telemetry, not an audit log or authentication mechanism.
