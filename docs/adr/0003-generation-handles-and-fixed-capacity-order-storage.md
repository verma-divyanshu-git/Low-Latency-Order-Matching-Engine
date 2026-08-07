# ADR-0003: Generation handles and fixed-capacity order storage

- Status: Accepted
- Date: 2026-08-08

## Context

Orders need stable references without exposing owning pointers or allowing a released slot to be mistaken for a later order.
The matching path must also avoid allocation after startup.
The 32-byte order record should contain only fields needed by order-book operations, while ownership metadata remains off that hot record.

## Decision

The order arena allocates one fixed array of slots at construction.
Each slot owns one 32-byte order plus generation, free-list, and liveness metadata that remains outside the order record.
Its capacity cannot grow.
A handle contains a slot index and generation.
Resolution and release validate the index, live bit, and generation before accessing or recycling a slot.
Generation zero is reserved as invalid, so generation wrap advances to one.
Normal exhaustion and invalid-handle paths return explicit errors without exceptions.
Pointers and references returned by resolution are borrowed only until that handle is released.
Retaining a borrowed pointer or reference after release can alias a future occupant and bypass generation validation.

## Alternatives

Owning pointers were rejected because they do not independently detect stale references and complicate fixed ownership.
Growing containers were rejected because later growth can allocate and invalidate storage.
Embedding generation and free-list metadata in each order was rejected because it would enlarge or displace the hot order fields.

## Consequences

The arena has bounded storage and deterministic slot reuse after construction.
Its acquire, resolve, and release operations cannot allocate or reallocate storage.
Stale, duplicate, and out-of-range releases are rejected at runtime.
Callers must choose capacity at startup and handle exhaustion.
Generation checks make accidental stale reuse unlikely, but a handle retained through a complete 32-bit generation cycle can alias a future occupant.
