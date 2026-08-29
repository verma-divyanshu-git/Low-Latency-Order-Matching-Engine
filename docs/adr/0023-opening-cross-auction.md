# ADR-0023: Opening-cross auction

## Status

Accepted.

## Decision

The engine supports an explicit construction-time opening-auction state.
While that state is active, it accepts GTC limit orders into the book without continuous matching.
Market, IOC, FOK, post-only, iceberg, and stop submissions are rejected because their auction-specific semantics are outside this implementation.
Cancel, quantity reduction, and replacement remain available.

A sequenced `uncross_opening_auction` command ends the auction exactly once.
The engine considers each price from the best ask through the best bid.
It first chooses the price that maximizes executable quantity, then minimizes absolute order imbalance, then minimizes distance from the local best-bid/best-ask midpoint.
A final tie selects the higher price.
Eligible buys execute from highest price to lowest price and eligible sells execute from lowest price to highest price, preserving FIFO within each level.
Every execution uses the single clearing price.
The engine enters continuous trading even when no shares cross.

This hierarchy follows the Nasdaq Opening Cross rule's maximum-paired-shares and minimum-imbalance priorities.
The engine does not ingest an external NBBO or prior official closing price, so its later tie breaks are explicit deterministic local rules rather than a claim of full Nasdaq conformance.

The command uses the existing fixed 36-byte payload with canonical zero unused fields.
Snapshot format v6 stores the trading state, and versions 1 through 5 restore continuous trading.
Crossed-book invariants are permitted only while opening-auction state is active.

## Consequences

Auction accumulation, clearing-price selection, execution, journaling, snapshot restore, and replay are deterministic and integer-only.
The production clearing scan is linear in configured price levels plus generated matches.
The opening auction does not support imbalance messages, auction-only order types, collars, external reference prices, or reopening.

## Rule source

Nasdaq Equity 4, Rule 4752, "Opening Process," defines the Nasdaq Opening Cross and its price-selection priority.
https://listingcenter.nasdaq.com/rulebook/nasdaq/rules/Nasdaq%20Equity%204
