# ADR-0026: Strict runtime configuration and API compatibility

## Status

Accepted.

## Decision

Runtime configuration is a complete list of canonical `key=value` entries parsed with `std::from_chars`.
Every field is required exactly once.
Unknown fields, malformed entries, noncanonical numbers, overflow, invalid paths, and unsafe resource relationships fail before queues, books, journals, or snapshots are constructed.

The event queue must hold the engine's maximum atomic event batch.
Gateway limits and price collars derive from the same validated values used for matcher sizing.
No environment fallback, implicit default, clamping, floating point, secret field, or partial-success configuration is supported.

The public runtime API uses explicit major and minor constants.
A consumer is compatible only when its major matches and its requested minor is no newer than the library minor.
Persisted format versions remain separate.

## Consequences

Startup either receives one complete internally consistent configuration or fails with a field-specific error.
Adding a required field is an API compatibility change.
Operational deployment systems may translate their native config format into canonical entries, but the engine boundary remains deterministic and dependency-free.
