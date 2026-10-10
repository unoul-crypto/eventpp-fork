# Collecting and aggregating handler returns

`EventDispatcher::dispatchWithResults` returns both individual handler values and an
optional value computed by an application-defined aggregator for the event. Ordering,
argument plans, filters and cancellation use the same dispatch logic as `dispatch`.

## C++11 example

```cpp
#include <eventpp/eventdispatcher.h>
#include <numeric>

using Dispatcher = eventpp::EventDispatcher<int, int(int)>;

void example() {
    Dispatcher events;
    events.appendListener(1, [](int value) { return value + 1; });
    events.appendListener(1, [](int value) { return value * 2; });
    events.setResultAggregator(1, [](const std::vector<int> & values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });

    auto result = events.dispatchWithResults(1, 5);
    // result.results == {6, 10}
    // result.handles identifies the corresponding subscriptions.
    // *result.aggregate == 16
}
```

## Public types and functions

- `ListenerResult`: the handler return type with references and top-level qualifiers
  removed (`std::decay`).
- `ListenerResults`: `std::vector<ListenerResult>`.
- `AggregationResult`: `Policies::AggregationResult`, or `ListenerResult` by default,
  stored as a value with references and top-level qualifiers removed.
- `ResultAggregator`: `std::function<AggregationResult(const ListenerResults &)>`.
- `DispatchResult`: owns `ListenerResults results`, `std::vector<Handle> handles`,
  and `std::unique_ptr<AggregationResult> aggregate`.
  It also contains `std::vector<ListenerError> errors`; each error has `handle` and
  `std::exception_ptr exception`. See [listener exception policies](listener_errors.md).
  `bool stoppedByResult` is initially false and records a stop requested by a
  result continuation function.

```cpp
void setResultAggregator(const Event &, const ResultAggregator &);
void clearResultAggregator(const Event &);
DispatchResult dispatchWithResults(Args...);
DispatchResult dispatchWithResults(eventKey, Args...);
DispatchResult directDispatchWithResults(const Event &, Args...);
```

The dispatch overloads follow the existing `ArgumentPassingMode` and `getEvent`
policies. `directDispatchWithResults` bypasses `getEvent`, using the supplied key.
All dispatch functions are `const`. Setting an empty aggregator clears it. Aggregators
can be registered before listeners exist and are independent of ordering/planner configuration.

The two vectors have matching positions: `handles[i]` is the subscription whose
callback returned `results[i]`. A returned handle can already be expired if that
subscription removed itself or was removed later during dispatch; its ownership identity
still identifies the subscription.

`aggregate` is empty without a configured aggregator. Otherwise, dereference it to
access the aggregate value, or move it out. `DispatchResult` is move-only. This optional
storage supports non-default-constructible and move-only aggregate types in C++11.

## Continue or stop after each result

`setResultContinuation(event, function)` configures a per-event function with the
signature `bool(const Handle &, const ListenerResult &, const T &...)`, where each
`T` is the corresponding callback argument type with references removed. It runs
after each successful handler return. Return true to continue or false to stop
invoking listeners for this dispatch. `clearResultContinuation(event)` or setting
an empty function restores the usual behavior without clearing ordering, plans,
exception policy or aggregation.

```cpp
using Dispatcher = eventpp::EventDispatcher<int, int(int)>;
Dispatcher dispatcher;
dispatcher.appendListener(1, [](int) { return 0; });
dispatcher.appendListener(1, [](int) { return 7; });
dispatcher.appendListener(1, [](int) { return 9; });
dispatcher.setResultContinuation(1,
    [](const Dispatcher::Handle &, const int & result, const int &) {
        return result == 0; // Stop at the first successful answer.
    });
auto report = dispatcher.dispatchWithResults(1, 10);
// report.results == {0, 7}, report.stoppedByResult == true.
```

The stopping handler's result and handle are retained. An aggregator receives the
collected prefix, including that result. `stoppedByResult` means the function
returned false, even if that was the last listener. Stops from filters or the
existing `canContinueInvoking` policy do not set this flag. When the continuation
returns true, the existing argument-based continuation policy is checked as usual.

The same function applies to ordinary `dispatch` and ordinary queued `enqueue`
processing. Those paths observe a local owned value without allocating result
vectors or invoking an aggregator. The API has the same collectable-return
requirement as `dispatchWithResults`: void and non-ownable returns cannot configure
it; copyable reference returns are observed as copied values. Move-only results are
passed by const reference. Arguments reflect their state after the callback and
use each plan entry's replacement values when present. References passed to the
function must not be retained beyond that call.

Failed handlers under `Continue` have no result and do not invoke the continuation.
Exceptions from the continuation itself are infrastructure failures: they propagate
from synchronous dispatch and ordinary queue processing, or fail the future of a
queued report/task. They are not converted into handler `errors`, and aggregation
does not run after such a failure.

Configuration is captured with ordering and exception policy, after before-dispatch
mixins/filters and before listener selection or invocation. Changes made by a
handler or continuation affect later and nested dispatches, not the current
snapshot. The function executes outside internal locks and may reenter the
dispatcher. Concurrent dispatches can call the same function simultaneously; shared
application state requires synchronization. Copies copy the callable's state;
move and swap transfer it with the other event configuration. Captured pointers
and references retain their normal sharing semantics.

## Different aggregate types

```cpp
struct Policies {
    using AggregationResult = std::string;
};
using Dispatcher = eventpp::EventDispatcher<int, int(), Policies>;

void configureAggregator(Dispatcher & events) {
    events.setResultAggregator(1, [](const std::vector<int> & values) {
        return std::to_string(values.size());
    });
}
```

The aggregate type is configured for the dispatcher at compile time. Each event can
have a different aggregator function, with that same result type. The aggregator observes
the vector through a const reference and cannot consume its elements; both the vector
and its aggregate remain available to the caller.

## Execution semantics

- Results follow the actual callback order, including `ListenerOrder` and `ListenerPlan`.
  Skipped, invalid, duplicate and removed handles do not contribute results.
- A plan entry with replacement arguments contributes the return from that invocation.
  The return is stored before the plan or its owned arguments are destroyed.
- After storing each completed return, the result continuation may stop dispatch.
  If it continues, the existing cancellation policy checks that callback's actual
  arguments. Previously collected returns are retained.
- The aggregator is captured before filters or callbacks execute and called exactly once
  after successful processing, outside internal locks. Empty vectors are also aggregated,
  including events without listeners, empty plans and events rejected by filters.
- Changes to the configured aggregator during dispatch affect subsequent and nested
  dispatches. They do not replace the aggregator captured by the current dispatch.
- Nested and concurrent collections own separate result vectors. The same aggregator
  callable can execute concurrently, so mutable application state needs synchronization.
- Copying a dispatcher copies its aggregator callable, just as its selection callable.
  Moving and swapping transfer aggregator configuration with the listeners.

By default, callback, selection, collection-allocation, cancellation-policy and aggregator
exceptions propagate normally. With the opt-in per-event `Continue` policy, handler
failures are recorded in `errors` and invocation continues; aggregation receives only
successful values. Other failures retain propagation. If processing throws, the aggregator
is not called, collected values are cleaned up, and no partial `DispatchResult` is returned. If the aggregator throws,
its exception propagates and collected values are also cleaned up. The dispatcher remains
usable; application callback side effects are not undone.

## Supported returns and lifetime

Collection is available for non-void callback returns that can be stored as values.
Move-only value returns, such as `std::unique_ptr<T>`, are supported. A reference return
is copied into the vector, so a returned reference to a plan-owned `std::string` becomes
an independent string before the plan is destroyed.

Void handlers and references to abstract or non-copyable objects cannot be collected
this way. Their ordinary `dispatch` and argument plans remain supported. Pointers and
types which themselves contain references retain their usual lifetime rules: storing a
pointer does not copy the object it points to. Array references decay to pointers.

An aggregator must produce a non-void value that can be moved or copied into its owned
storage. It must not retain references to the input vector or rely on them after dispatch.

## EventQueue and ordinary dispatch

`EventQueue` inherits the synchronous `dispatchWithResults` API. It immediately invokes
listeners and returns the vectors and aggregate, without enqueueing an event.

For queued work, [enqueueWithResults](queue_results.md) returns a `std::future<DispatchResult>`.
The queue's process methods collect and aggregate returns for those events and complete
the future. Events submitted with ordinary `enqueue` still discard returns and do not run
result aggregators. Ordinary synchronous `dispatch` also keeps that behavior.

Collection allocates the two result vectors and, when configured, storage for the aggregate.
The vectors reserve a capacity hint from the current listener count, bounded by the
returned selection/plan size when selection is enabled. This avoids repeated growth for
full dispatches. The hint does not limit traversal or change subscription mutation rules;
concurrent additions can still require vector growth. Removal, invalid handles or early
cancellation can leave unused capacity in the returned vectors.
Ordinary dispatch does not allocate these vectors or look up aggregator configuration.
