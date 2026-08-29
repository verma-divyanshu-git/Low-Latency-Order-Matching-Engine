# ADR-0022: Threshold pro-rata with FIFO residue

## Status

Accepted.

## Decision

The engine supports a construction-time choice between FIFO and threshold pro-rata allocation.
Threshold pro-rata operates independently at each executable price level.
It calculates each resting order's initial share from that order's displayed quantity and the incoming quantity available when allocation begins.
The calculation uses checked bounds and wide integer multiplication, so it neither uses floating point nor overflows the supported quantity range.

An initial share below the configured minimum is discarded.
After the proportional pass, any unallocated quantity executes in price-level FIFO order.
Iceberg orders participate using only their displayed quantity, and replenishment retains its existing loss-of-priority behavior.
Self-trade prevention stops both the proportional and FIFO passes at the first protected resting order.

Snapshot format v5 stores the allocation mode and minimum quantity.
Older snapshots restore FIFO mode.
The sequenced engine, independent reference model, differential simulator, and snapshot restore path use the same construction-time configuration.

## Consequences

Allocation is deterministic, integer-only, and allocation-free in the matching path.
The minimum can leave more quantity for the FIFO residue pass than pure pro-rata would.
This implementation does not model lead-market-maker, top-order, or configurable weighting variants.

## Rule source

CME Group documents Pro-Rata, Configurable, Threshold Pro-Rata, and Threshold Pro-Rata with LMM as distinct supported matching algorithms.
This engine implements the unweighted threshold variant and explicitly defines FIFO residue as its deterministic tie and remainder policy.
https://www.cmegroup.com/education/matching-algorithm-overview.html