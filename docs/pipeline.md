# Durable three-stage pipeline

Phase 5A connects durable command ingress, single-writer matching, and ordered event publication with two bounded SPSC queues.
The stages do not create threads or wait.
Applications and tests own scheduling, affinity, yielding, shutdown, and watchdog policy.

## Queue contract

`SpscQueue<T>` accepts non-pointer, trivially copyable, default-constructible values and allocates its exact runtime capacity once.
Capacity is between 1 and 1,000,000 inclusive.
Move-only producer and consumer endpoints can each be claimed once before concurrent use.
The producer owns the local tail and the consumer owns the local head, so queue operations need no compare-and-swap, lock, or internal spinning.
Unsigned monotonic index subtraction remains correct across `uint64_t` wrap because occupancy never exceeds the bounded capacity.

Producer and consumer state use explicit 128-byte alignment.
This deliberately conservative value avoids relying on implementation-defined `std::hardware_destructive_interference_size` warnings while separating independently written cache lines.
The producer initializes a payload and then release-stores the published tail.
The consumer acquire-loads that tail before reading the payload.
The consumer release-stores the published head after reading.
The producer acquire-loads that head before reusing the slot.
Cached remote indexes avoid an acquire load on every successful operation.

`empty()` is observational under concurrency and may become stale immediately.
Producer `available()` is exact at the instant of its acquire head load.
The queue exposes no slot pointers or reservations.

## Pipeline ordering and failures

`DurableIngressStage` checks queue capacity and obvious journal full or poison state before sequencing.
It stamps, durably appends, and only then publishes the command.
Queue backpressure and journal-full status consume neither sequence nor journal capacity.
Any append failure after stamping poisons ingress.
`commit_indeterminate` is returned as `persistence_required`, leaves the command unpublished, and requires journal recovery before another ingress instance continues.
Therefore every command the matcher can apply is present in the journal.

`MatchingStage` peeks without releasing the command.
It asks `SequencedEngine` for the command's exact conservative event-capacity requirement and checks producer availability before mutation.
The engine applies into construction-time scratch storage, exact ordered events are published, and only then is the command released.
Output backpressure leaves the command and engine untouched.
An invalid journal command or sequence/apply mismatch poisons the stage rather than skipping input.

`PublicationStage` provides allocation-free ordered pop and callback drain operations.
Events retain command sequence and event index.

## Snapshots and shutdown

Matching-stage snapshot persistence is owner-thread-only and must be invoked between `process_one` calls.
There is no active command reservation at that boundary, so a snapshot never observes half-applied or half-published work.
Persistence may stall the matching owner and is intentionally excluded from matching-only benchmark intervals.

Clean shutdown stops ingress, drains the command queue through matching, then drains the event queue through publication.
The deterministic threaded test uses bounded operation counts and externally owned `std::jthread`s.

## Benchmarks

`spsc_queue_benchmark` reports single-thread push/pop loop overhead separately from two-thread handoff throughput.
`pipeline_throughput_benchmark` reports an explicitly configured three-thread rejected-market workload and includes mmap journal synchronization and file synchronization on every command.
Neither executable pins CPUs internally.
Their JSON output is a local artifact, not a publishable performance claim.

## References

- [Rigtorp SPSCQueue design notes](https://github.com/rigtorp/SPSCQueue)
- [1024cores bounded SPSC queue](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-spsc-queue)
- [C++ working draft: data races and happens-before](https://eel.is/c++draft/intro.races)
