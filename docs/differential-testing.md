# Differential order-book simulation

Phase 2B runs the production order book and a deliberately simple reference book through identical deterministic command streams.
The reference book uses `std::map` price levels, FIFO `std::deque` queues, and linear token scans.
It does not use production matching helpers, intrusive indexes, `OrderArena`, or `HierarchicalBitmap`.

After every command, the driver compares normalized outcomes, ordered trades, BBO, every level in the 101-tick domain, every tracked live order, and production structural invariants.
Engine handles map to unrelated model tokens, so representation-specific values are not compared.
The model validates each fill against the current aggressor and a live opposite-side maker with sufficient pre-trade quantity.

The independent ledger checks:

`accepted submitted quantity = 2 * traded quantity + explicitly canceled or unfilled quantity + resting quantity`

An accepted replacement cancels the old remainder and submits the replacement quantity.
An accepted amend decrease contributes the removed quantity to cancellation.
IOC and market residuals are canceled, while rejected FOK quantity is not accepted.
Test arithmetic uses portable checked `std::uint64_t` addition and doubling.
Any arithmetic overflow fails the simulator with its seed and step instead of wrapping.

The normal regression corpus contains these fixed 64-bit seeds and runs 1,000 commands per seed:

- `2611923443488327891`
- `1376283091369227076`
- `11820040416388919760`
- `589684135938649225`
- `4983270260364809079`
- `13714699805381954668`
- `13883517620612518109`
- `4577018097722394903`
- `10526836309316205339`
- `15073842237943035308`

The suite also labels high-cancel and alternating-price-regime workloads as synthetic stress tests.
These mixes target lifecycle and price-movement edge cases and are not claims about universal market realism.

Failures include the seed, step, printable pointer-free command, scenario, and a replay command.
Replay one normal seed with:

```sh
ORDER_BOOK_DIFF_SEED=2611923443488327891 \
  ./build/debug/matching_engine_core_tests \
  --gtest_filter=OrderBookDifferentialTest.NormalSeedRegressionCorpusCoversTenThousandOperations
```

`ORDER_BOOK_DIFF_SEED` accepts only a complete unsigned decimal `uint64_t` value.
Empty, signed, overflowing, whitespace-padded, malformed, and trailing-character values fail the test and never fall back to the default corpus.

Differential agreement is strong evidence that two independent implementations satisfy the exercised state-machine semantics.
It does not prove correctness outside the generated command domain, thread safety, gateway behavior, persistence, self-trade prevention, latency, or the absence of a shared misunderstanding in the specification.
