# Resource limits

All capacities are fixed and validated before use.
The engine does not grow these structures on the matching hot path.

- Price domain: 1 through 1,000,000 integer price levels.
- Order quantity: 1 through `UINT32_MAX` per order.
- Order arena: at most 1,000,000 slots in the library.
- Production runtime order capacity: at most 499,999 because the event queue must hold `2 * max-orders + 1` events and SPSC capacity is at most 1,000,000.
- Command and event SPSC queues: 1 through 1,000,000 elements each.
- Journal segment: 1 through 1,000,000 records.
- Journal record: 80 bytes plus one 64-byte segment header, so a maximum segment is 80,000,064 bytes.
- Snapshot slots: at most 1,000,000.
- Snapshot v6 slot: 96 bytes plus a 124-byte header, so the maximum snapshot is 96,000,124 bytes.
- Command payload: 36 bytes; command frame: 40 bytes.
- Engine event and canonical MBO record: 64 bytes.
- Market-data frame: 64 bytes; BBO frame: 40 bytes.
- Runtime lanes: positive and no greater than command queue capacity.

Memory use also includes two `PriceLevel` arrays, hierarchical occupancy bitmaps, order metadata arrays, queue storage, event scratch storage, gateway active-order storage, and process and standard-library overhead.
Operators must size from measured resident memory on the target ABI rather than summing serialized sizes alone.

Capacity exhaustion is explicit.
The runtime reports queue backpressure, journal capacity, event capacity, and order capacity rather than allocating or dropping work.
Journal rotation handles a full segment, but disk-space exhaustion remains an I/O failure requiring operator action.
