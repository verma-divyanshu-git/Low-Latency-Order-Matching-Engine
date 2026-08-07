# ADR-0005: Single-writer matching and fixed caller-owned output

- Status: Accepted
- Date: 2026-08-08

## Context

The matching core needs deterministic price-time priority without synchronization or allocation on its hot path.
It must also report every execution without partially mutating the book when output storage or order capacity is insufficient.

## Decision

One matching thread exclusively owns each `OrderBook`.
Bid and ask level arrays, occupancy bitmaps, and the order arena are fixed at construction.
Orders at one price form an intrusive FIFO, and the opposite best price is always consumed first.
Executions use the resting maker's price.
These semantics follow Nasdaq price/display/time priority and execution-price rules, while the in-process request and trade records remain project-specific.

The caller supplies a trade span with at least `max_orders` slots.
Submissions with less output capacity are rejected before mutation.
This conservative bound is sufficient because one incoming order can execute against at most every live arena order.

A full arena rejects a limit order before mutation only when it cannot cross the current opposite best.
If it crosses, the incoming order either fully executes or exhausts at least one maker before any residual rests, so a slot is available.
This preflight avoids mutation followed by pool-exhaustion failure without a second matching simulation.

Market residual is cancelled rather than rested.
Duplicate `OrderId` detection remains a gateway responsibility, so the matcher does not allocate or maintain an identifier map.

## Consequences

Matching and observability operations do not allocate after construction.
Normal validation and capacity failures are explicit results rather than exceptions.
The owner must serialize all access, provision output storage, and keep any returned generation handle for later cancellation.
Memory use is proportional to the configured price domain and order capacity.

## Primary references

- [Nasdaq Equity 4 Rule 4757, Book Processing](https://listingcenter.nasdaq.com/rulebook/nasdaq/rules/Nasdaq%20Equity%204)
- [Nasdaq OUCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf)
- [C++ working draft: data races](https://eel.is/c++draft/intro.races)
