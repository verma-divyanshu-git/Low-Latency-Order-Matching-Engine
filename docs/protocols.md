# Protocols

## Command frame

The command frame is a fixed 40-byte little-endian envelope.
Byte 0 is protocol version 1.
Bytes 1 through 3 are reserved zero.
Bytes 4 through 39 contain the canonical 36-byte `CommandPayload` encoding.

The decoder rejects a wrong length, unsupported version, nonzero reserved header bytes, and invalid or noncanonical payloads.
The command sequencer accepts caller-supplied logical time only.
It never reads a clock.

## Market-data frame

The market-data frame is a fixed 64-byte little-endian envelope.
Byte 0 is protocol version 1, byte 1 is message type, byte 2 is side, bytes 3 through 7 are reserved zero, and bytes 60 through 63 are reserved zero.
The remaining fields are sequence, primary and secondary order IDs, price ticks, primary and secondary quantities, and order count.

The decoder rejects malformed length, unsupported version, unknown type, invalid side where a side is required, nonzero reserved bytes, and noncanonical unused fields.
`MarketDataInputStream` rejects discontinuous source sequences before adaptation.
The gateway-backed adapter rejects invalid input before assigning a matcher sequence.

Supported matcher mutations are add-order, delete-order, and replace-order.
The adapter records canonical engine events to map a feed order ID to its generation-checked resting handle.
Delete maps to cancel.
Replace maps to amend quantity because the current normalized replace frame reserves its price field.

## Published frames

BBO publication uses a fixed 40-byte little-endian frame with version 1 and explicit bid and ask presence bits.
MBO publication uses the canonical fixed 64-byte engine-event frame.
The MBO publisher does not reinterpret event semantics.

## Decoder fuzzing

`protocol_decoder_fuzz` dispatches arbitrary byte input to the public command, BBO, market-data, and engine-event decoders.
Every accepted value must re-encode as a canonical frame.
The `fuzz` CMake preset runs a bounded 10,000-execution smoke test for this harness and the order-book differential fuzzer.

## Replay contract

`replay_market_data` reads validated fixed frames, adapts through the gateway before sequencing, applies accepted commands to `SequencedEngine`, and fingerprints the canonical event stream.
It returns an error on input, adapter, apply, or invariant failure.
No partial-success result is returned after an error.