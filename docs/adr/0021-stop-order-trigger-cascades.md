# ADR-0021: Stop-order trigger cascades

## Status

Accepted.

## Decision

The engine supports stop-market and stop-limit orders.
A stop remains dormant in the shared fixed-capacity order arena until the last execution price reaches its trigger.
Buy stops trigger when the last execution price is at or above the trigger price.
Sell stops trigger when the last execution price is at or below the trigger price.

Triggered stop-market orders execute as market orders.
Triggered stop-limit orders execute at their configured limit and rest any unfilled quantity at that limit.
Eligible stops activate in submission order.
Each activation may update the last execution price and trigger another scan.
The cascade is bounded by the arena capacity because every dormant stop occupies one arena slot and can activate at most once.

The command payload carries the full signed 64-bit trigger price in the existing handle fields.
Snapshot format v4 stores the last execution price, dormant-stop linkage, trigger price, and optional limit price.
Earlier snapshot versions remain readable and contain no dormant stops.

The sequenced engine reserves one result event, at most one trade event per arena slot, and one activation event per dormant stop.
Its construction-time maximum is therefore twice the arena capacity plus one.
Runtime preflight uses the current dormant-stop count so commands retain the tighter historical bound when no stops are present.

## Consequences

Trigger processing is deterministic, allocation-free in the matching path, and replayable from snapshots and journals.
The implementation models last-trade triggers only.
It does not model exchange-specific protection bands, election messages, or alternate trigger sources.

## Rule source

This follows CME Globex order-type semantics for stop and stop-limit orders, where buy stops are elected at or above the trigger price and sell stops at or below it.
The engine deliberately omits CME's stop-with-protection price band.
https://www.cmegroup.com/confluence/display/EPICSANDBOX/Supported+Order+Types