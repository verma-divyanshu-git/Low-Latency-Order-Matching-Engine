# ADR-0019: Canonical MBO and MBP publication

- Status: Accepted
- Date: 2026-08-29

## Context

Downstream consumers need order and price-level views without duplicating matching logic or inferring aggregates from incomplete event fields.

## Decision

MBO serializes each canonical `EngineEvent` with the fixed event codec.
MBP uses the same canonical event as its publication trigger and queries the authoritative post-apply `OrderBook` for the selected best price level.
MBP emits a normalized market-data level-update frame, including an explicit empty level when that side has no best price.

## Consequences

MBO preserves matcher event semantics exactly.
MBP never invents aggregate quantity or order count from an event that lacks complete level information.
The publishers are allocation-free after construction and report encoding errors explicitly.

## References

- [NASDAQ TotalView-ITCH specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)