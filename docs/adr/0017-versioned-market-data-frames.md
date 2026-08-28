# ADR-0017: Versioned market-data frames

- Status: Accepted
- Date: 2026-08-29

## Context

Replay input needs a portable, fixed-format representation that can reject malformed data before it reaches sequencing or matching.

## Decision

Use a fixed 64-byte little-endian market-data frame with explicit version, type, side, sequence, order IDs, price, quantities, count, and reserved bytes.
The decoder rejects wrong length, unsupported version, unknown type, invalid side where required, nonzero reserved bytes, and noncanonical unused fields.
The input stream rejects source-sequence gaps before adaptation.

## Consequences

Frame layout is stable for golden tests, fuzzing, and deterministic replay.
Native C++ object layout is never serialized.
Malformed input cannot advance the adapter or mutate the matching engine.

## References

- [NASDAQ TotalView-ITCH specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)
- [C++ object model](https://eel.is/c++draft/intro.object)