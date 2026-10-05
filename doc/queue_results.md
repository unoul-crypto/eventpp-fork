# Queued results with enqueueWithResults

`EventQueue::enqueueWithResults` queues an event and returns a
`std::future<DispatchResult>`. The result has the same `results`, `handles` and optional
`aggregate` fields as [synchronous result collection](return_results.md), plus handler
`errors` when the event uses the opt-in `Continue` policy. See
[listener exception handling](listener_errors.md). `enqueueWithReport` uses the same
future and lifetime rules and also supports `void` and non-ownable returns.

## C++11 example

```cpp
#include <eventpp/eventqueue.h>
#include <numeric>

void example() {
    eventpp::EventQueue<int, int(int)> queue;
    queue.appendListener(1, [](int value) { return value + 1; });
    queue.appendListener(1, [](int value) { return value * 2; });
    queue.setResultAggregator(1, [](const std::vector<int> & values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });

    auto future = queue.enqueueWithResults(1, 5);
    queue.process();
    auto result = future.get();
    // result.results == {6, 10}
    // *result.aggregate == 16
}
```

Enqueueing does not invoke callbacks and does not start a worker thread. Process the
queue before calling `get`, or arrange for another thread to process it. Standard
`future.wait`, `future.wait_for` and `future.get` provide completion access; `get` consumes
the future once.

## API and processing

`ResultFuture` is `std::future<DispatchResult>`. `enqueueWithResults` follows the same
argument-count overloads, `getEvent` policy and `ArgumentPassingMode` as `enqueue`:
the event key can be separate from the callback arguments or extracted from them.
Callback returns must be collectable non-void values, as for `dispatchWithResults`.

Arguments are stored with the existing queue's value/move semantics. Listeners, metadata,
selection/planning configuration and the aggregator are read at processing time. For
example, `setListenerMetadata` after enqueueing affects the eventual result.

`process`, `processOne`, `processIf` and `processUntil` all complete futures for the result
events they dispatch. Events skipped by a processing predicate remain queued and their
futures remain pending. Empty listener lists and empty plans produce empty vectors;
an aggregator, if present, still receives that empty vector.

Ordinary and result-bearing events can be mixed in the same queue and use the configured
`QueueList` ordering. Ordinary `enqueue` events keep discarding returns and do not run
aggregators. Result-bearing events use a shared promise state; ordinary events do not
allocate that state. Each queued-event record has an additional shared pointer field.

## Exceptions and cancellation

By default, for result-bearing events, callback, filter, selector/planner and aggregator
exceptions are stored in the future. `future.get` rethrows the original exception. With
the event's opt-in `Continue` policy, only handler failures become report `errors` and
processing invokes the remaining handlers; infrastructure failures still fail the future.
Processing can continue with subsequent queued events, and no partial result is returned
for a failed event.

For ordinary events, exceptions still propagate from the process method, except for
handler failures discarded by the opt-in `Continue` policy. If such an
exception discards a processing batch, any unprocessed result-bearing events in that
batch are canceled and their futures complete with `std::future_errc::broken_promise`.
Exceptions from processing predicates also propagate and cancel their abandoned batch.

`clearEvents` cancels result-bearing events still in the queue. Queue destruction also
cancels its pending queued events. Their futures become ready and `get` throws
`std::future_error` with the `broken_promise` code, even if a peek copy of the queued event
still exists. Work already claimed for execution is not interrupted. Events selected
into a process method's local batch are no longer in the pending queue and are not
removed by a concurrent or nested `clearEvents` call.

Discarding the returned future does not cancel queued execution.

## peekEvent and takeEvent

A result-bearing queued event and its copies share the same completion state. Dispatching
a saved event from `peekEvent` or `takeEvent` completes its future. Only one dispatch can
claim that state: later dispatches of another copy or of the original queued record skip
the event without repeating callbacks. This also prevents execution after cancellation.
Ordinary queued events retain their existing repeat-dispatch behavior.

`takeEvent` transfers the record out of the queue. It remains pending until manually
dispatched or abandoned. Destroying all saved copies of undispatched taken work completes
its future with `broken_promise`. Destroying the original queue does not cancel work
already taken out of it. A mutable saved event can be dispatched with mutable-reference
callback arguments; a const saved event requires arguments compatible with its const view.
As before, `peekEvent` requires copyable arguments, while `takeEvent` supports movable ones.

Queue copying, moving and assignment retain the existing library rule: they operate on
listener configuration, not pending queued records. Pending futures stay associated with
the queue or saved event that owns the work; they are not duplicated in a copied queue.

The standard future can be consumed on a different thread. Queue execution and state
claiming use the queue's `Threading` policy; a single-thread policy still requires queue
operations to be serialized by the application.
