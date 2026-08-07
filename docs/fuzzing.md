# Order book fuzzing

The `order_book_fuzz` target uses libFuzzer and AddressSanitizer to compare the production order book with the independent standard-library reference model.
It is defensive correctness testing, not a proof of correctness.
A mismatch or failed production invariant aborts so libFuzzer preserves the reproducing input.

## Byte grammar and limits

The harness reads at most 448 input bytes as at most 64 complete seven-byte commands.
Trailing bytes that do not form a complete command are ignored.
Every byte access follows a complete-command size check.
Order identifiers are generated monotonically from one, rather than read from input, so every submitted operation has a deterministic unique identifier.

Each command is:

1. Operation: byte modulo 5 selects limit, market, cancel, amend, or replace.
2. Side: byte modulo 4 selects buy, sell, or one of two invalid enum values.
3. Price: byte modulo 12 selects one of eight valid ticks, one tick below the domain, one tick above it, `INT64_MIN`, or `INT64_MAX`.
4. Quantity: byte modulo 20 selects zero, 1 through 16, 17, or `UINT64_MAX`.
5. Time in force: byte modulo 5 selects GTC, IOC, FOK, or one of two invalid enum values.
6. Trade output capacity: byte modulo 4 selects the required eight slots, zero slots, seven slots, or eight slots.
7. Target: selects a tracked live order when possible or an invalid handle.

Fields that an operation does not consume still occupy their byte.
This fixed-width grammar keeps mutation and manual reproduction simple while exercising normal and rejected side, time-in-force, price, quantity, output-capacity, cancellation, amendment, and replacement paths.

After every complete command, the harness compares the exact result fields and ordered trades.
It also compares best bid and ask, every level in the bounded domain, every tracked live order, live-order count, and the production `check_invariants()` result.

## Corpus

Six small non-sensitive seeds live in `tests/fuzz/corpus/order_book`:

- `passive-crossing-fifo` covers passive orders, crossing, and FIFO execution.
- `ioc-residual` covers immediate execution with residual cancellation.
- `fok-sufficient` covers a fully fillable FOK order.
- `fok-insufficient` covers a rejected FOK order without book mutation.
- `cancel-amend-replace` covers valid and invalid lifecycle targets.
- `boundaries-invalid` covers invalid side, time in force, prices, quantities, and output capacities.

The corpus contains only synthetic command bytes and no credentials or production data.

## Build and run

The fuzz preset requires upstream Clang with libFuzzer and AddressSanitizer.
On Apple silicon, install Homebrew LLVM and run:

```sh
cmake --preset fuzz
cmake --build --preset fuzz
ctest --preset fuzz -L fuzz
cmake -E copy_directory tests/fuzz/corpus/order_book build/fuzz/corpus/order-book
./build/fuzz/order_book_fuzz build/fuzz/corpus/order-book -runs=100000 -max_len=448 -timeout=10
./build/fuzz/order_book_fuzz tests/fuzz/corpus/order_book/* -runs=1 -max_len=448 -timeout=10
```

The CTest smoke test is bounded to 10,000 executions and 120 seconds.
Only `order_book_fuzz` links the libFuzzer runtime.
Other targets in the fuzz build receive coverage and AddressSanitizer instrumentation through `fuzzer-no-link`, while normal presets do not receive or link fuzz instrumentation.

To replay one seed or a libFuzzer artifact:

```sh
./build/fuzz/order_book_fuzz path/to/input -runs=1 -max_len=448 -timeout=10
```

## Sanitizers and CI budget

The regular Debug and Release suites run independently of fuzzing.
Dedicated presets cover AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer locally.
The fuzz preset combines libFuzzer coverage instrumentation with AddressSanitizer.

GitHub Actions keeps Release tests on Ubuntu and macOS.
Separate public Ubuntu jobs run the complete test suite under AddressSanitizer and UndefinedBehaviorSanitizer, plus a 10,000-execution Clang fuzz smoke.
The jobs use GitHub-hosted runners, apt packages, read-only repository permissions, no secrets, and explicit timeouts.
ThreadSanitizer remains local because support and reliability vary across hosted Ubuntu toolchains.
No scheduled million-execution job is configured, avoiding recurring CI consumption and ambiguity for forks or private mirrors.

## Limitations

The grammar is structure-aware but intentionally small.
It does not generate duplicate order identifiers, concurrent calls, arbitrary constructor configurations, process failures, or gateway behavior.
The production engine and reference model still share public value types and semantic assumptions, so correlated specification mistakes remain possible.
The bounded domain and capacity improve execution density but do not represent every deployment size.
Sanitizers and differential fuzzing can find defects in exercised paths; they do not establish absence of defects.
