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
- The cancellation policy is checked after storing each completed callback's return,
  using that callback's actual arguments. Previously collected returns are retained.
- The aggregator is captured before filters or callbacks execute and called exactly once
  after successful processing, outside internal locks. Empty vectors are also aggregated,
  including events without listeners, empty plans and events rejected by filters.
- Changes to the configured aggregator during dispatch affect subsequent and nested
  dispatches. They do not replace the aggregator captured by the current dispatch.
- Nested and concurrent collections own separate result vectors. The same aggregator
  callable can execute concurrently, so mutable application state needs synchronization.
- Copying a dispatcher copies its aggregator callable, just as its selection callable.
  Moving and swapping transfer aggregator configuration with the listeners.

Callback, selection, collection-allocation, cancellation-policy and aggregator exceptions
propagate normally. If processing throws, the aggregator is not called, collected values
are cleaned up, and no partial `DispatchResult` is returned. If the aggregator throws,
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
Ordinary dispatch does not allocate these vectors or look up aggregator configuration.
