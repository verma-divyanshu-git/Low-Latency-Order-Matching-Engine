# ADR-0008: Structure-aware differential fuzzing

## Status

Accepted.

## Context

The deterministic differential simulator already compares the production order book with an independent reference implementation over generated operation sequences.
Fixed regression seeds are valuable, but they explore only the sequences selected by the simulator.
Raw byte fuzzing without an operation grammar would spend most executions on malformed or shallow inputs and would rarely build meaningful book state.

The matcher has bounded public operations and explicit rejection results.
That makes a compact structure-aware byte grammar suitable for libFuzzer mutation while retaining rejected inputs as useful behavior.

Property-testing frameworks such as RapidCheck could provide generated values and shrinking.
Adding one now would introduce another test dependency and a second generation framework without replacing libFuzzer's coverage guidance, sanitizer integration, corpus persistence, or exact artifact replay.

## Decision

Add one test-only `LLVMFuzzerTestOneInput` executable for the existing fuzz preset.
It decodes fixed-width commands into public order-book operations and generates deterministic unique order identifiers.
Input bytes, command count, constructor capacity, price domain, quantity domain, and output buffers are bounded.

Each command runs against both the production order book and the existing independent standard-library reference model.
The harness compares exact operation results, ordered trades, best prices, all price levels, tracked live orders, live-order count, and production structural invariants after every operation.
Any mismatch terminates the process so libFuzzer records the input.

Only the harness executable links libFuzzer.
The fuzz build instruments supporting targets with `fuzzer-no-link` and AddressSanitizer.
Normal Debug, Release, and standalone sanitizer presets remain independent of the fuzzer runtime.

Keep six small synthetic corpus seeds for major lifecycle and rejection paths.
Run a bounded 10,000-execution smoke in CTest and public CI, with larger local campaigns used before changes are committed.

Defer RapidCheck until a concrete property needs richer typed generation or shrinking than the differential simulator and libFuzzer corpus provide.

## Consequences

Coverage-guided mutation can explore stateful operation sequences and preserve exact byte-level reproducers.
The independent model helps detect semantic divergence, while production invariants can detect structural corruption even when externally visible state still agrees.
AddressSanitizer runs in the same fuzz process.

The harness adds test-only standard-library allocation and linear scans, neither of which affects the production hot path.
Fixed limits trade deployment-scale coverage for deterministic, fast executions.
Agreement between two implementations is not proof that their shared interpretation of the intended market rules is correct.
