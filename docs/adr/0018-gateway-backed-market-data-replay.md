# ADR-0018: Gateway-backed market-data replay

- Status: Accepted
- Date: 2026-08-29

## Context

External feed messages identify orders by client order ID, while the matching core cancels and amends using generation-checked handles.
Input must be validated before sequence assignment.

## Decision

Replay reads validated frames through `MarketDataInputStream` and adapts every accepted mutation through `GatewayValidator` before `Sequencer::stamp`.
The adapter records canonical engine events to map a resting feed order ID to its generation-checked handle.
Add maps to limit submit, delete maps to cancel, and the normalized replace form maps to amend quantity.
Gateway IDs are released only after canonical result events confirm rejection or a successful cancel.

## Consequences

The core has no client-ID map.
Rejected input consumes neither matcher sequence nor engine state.
Stale deletes and replaces cannot target a recycled arena slot because the recorded handle includes its generation.

## References

- [NASDAQ OUCH specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/tradingproducts/ouch5.0.pdf)
- [C++ working draft: data races](https://eel.is/c++draft/intro.races)