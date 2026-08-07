# ADR-0006: Cancel, amend, replace, and time-in-force semantics

- Status: Accepted
- Date: 2026-08-08

## Context

The final Phase 1 slice needs deterministic lifecycle operations and immediate-execution instructions without adding identifier maps, allocation, or rollback paths.
Replacement must never remove a live order before every rejectable input has been validated.

## Decision

Resting orders are addressed by generation-checked handles.
Cancellation unlinks any FIFO position and releases its arena slot.
An equal or smaller remaining quantity is amended in place and keeps its handle and queue position.
Any replacement cancels the original and submits a new GTC order with the same identifier and side, so even a same-price replacement receives a new generation and joins the queue tail.
These priority choices follow the distinction in [SR-NASDAQ-2022-020](https://listingcenter.nasdaq.com/assets/rulebook/nasdaq/filings/SR-NASDAQ-2022-020.pdf) between size reductions that retain priority and changes that do not, together with the replace-order behavior described by [Nasdaq OUCH 5.0](https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf).

IOC executes immediately through eligible prices and cancels any residual.
FOK first sums price-level aggregates through the hierarchical occupancy index and rejects without mutation unless the complete quantity is available at the limit or better.
Successful FOK then executes fully using ordinary price/FIFO traversal and never rests.

## Consequences

All normal outcomes use fixed trivially-copyable records.
Duplicate order identifiers remain a gateway responsibility.
Replacement validation is atomic, but a successful replacement intentionally invalidates the old handle even when no quantity executes.
The implementation defines only this project's bounded in-process semantics and does not claim complete exchange protocol conformance.
