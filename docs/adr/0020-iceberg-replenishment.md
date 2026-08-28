# ADR-0020: Iceberg replenishment

## Status

Accepted.

## Decision

An iceberg order stores its total unfilled quantity and a configured displayed peak.
Only the displayed slice is executable at a time.
When that slice is fully consumed and hidden quantity remains, the engine replenishes the next slice and moves the order to the tail of its price-level FIFO queue.

The matching core stores peak and displayed quantities in fixed-capacity arrays parallel to the order arena.
This keeps the 32-byte `Order` representation stable and avoids hot-path allocation.
Trade output aggregates repeated fills of the same maker within one command, preserving the existing bounded output contract.

Snapshot format v3 records the configured peak and current displayed slice for each live order.
Earlier snapshot formats decode ordinary orders as fully displayed.

## Consequences

Replenished iceberg quantity loses queue priority to orders already waiting at that price.
The result is deterministic, allocation-free in the matching path, and survives snapshot recovery.

## Rule source

This follows Nasdaq Equity Rules, Rule 4702(b)(3), which specifies that replenished reserve quantity receives a new time priority.
https://listingcenter.nasdaq.com/rulebook/nasdaq/rules/nasdaq-equity-4