# Runtime operations

`RuntimeOperations` is a non-thread-owning control-plane state machine.
Applications feed it existing ingress, matching, and publication statuses from their owner threads.
It performs no I/O, allocation, waiting, or locking while observing statuses.

Lifecycle transitions are explicit:

- `starting` accepts one successful `start()` transition.
- `running` accepts ingress unless recovery or corruption has failed the runtime.
- `draining` begins after `begin_shutdown()` and rejects new ingress.
- `stopped` is reached only after both command and event queues are reported empty.
- `failed` is terminal for the runtime instance and requires recovery or restart.

Health severity is monotonic within one runtime instance:

- `healthy`
- `backpressured`
- `capacity_exhausted`
- `recovery_required`
- `corruption`

Queue backpressure and capacity events do not silently drop work.
Persistence ambiguity or recovery-required ingress fails the runtime and stops acceptance.
Invalid matcher commands, sequence mismatches, logical-time regressions, internal invariant failures, and poisoned matching state are classified as corruption and fail the runtime.

Counters saturate at `UINT64_MAX` rather than wrapping.
The hot path reads a trivially copyable `RuntimeHealthSnapshot`.
`runtime_health_json` is a separate off-hot-path serializer that may allocate and reports only fixed enum names and counters.
It accepts no free-form labels, paths, order identifiers, credentials, secrets, or personal data.
