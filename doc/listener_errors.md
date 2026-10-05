# Listener exceptions and dispatch reports

`EventDispatcher` and `EventQueue` support per-event C++ exception handling. Existing
calls retain propagation by default. Configure an event before dispatch or processing:

```cpp
dispatcher.setListenerExceptionPolicy(event, eventpp::ListenerExceptionPolicy::Continue);
// Restore the default:
dispatcher.setListenerExceptionPolicy(event, eventpp::ListenerExceptionPolicy::Propagate);
```

`Dispatcher::ExceptionPolicy` is an alias of `eventpp::ListenerExceptionPolicy`.
The policy is independent of ordering, argument planning and aggregation. Clearing
an ordering function or planner leaves the exception policy intact; resetting the
policy leaves selection intact. Copying, moving and swapping dispatchers includes
the policy configuration. Heterogeneous classes and standalone `CallbackList`
invocation retain their existing exception behavior.

## Results and errors

`DispatchResult` now has `std::vector<ListenerError> errors`, where:

```cpp
struct ListenerError {
    Handle handle;
    std::exception_ptr exception;
};
```

In `Continue` mode, failed handlers contribute an error rather than a return value.
`results[i]` still matches `handles[i]`, and both contain only successful returns.
Errors appear in failed-handler execution order. The two lists do not form a combined
execution log. An aggregator keeps its existing signature and receives only successful
values, including an empty vector when every handler fails. The exception pointer
owns the captured exception; the handle remains weak and may expire after removal.

```cpp
#include "eventpp/eventdispatcher.h"
#include <exception>
#include <numeric>
#include <stdexcept>

int main() {
    using Dispatcher = eventpp::EventDispatcher<int, int(int)>;
    Dispatcher dispatcher;
    auto broken = dispatcher.appendListener(1, [](int) -> int {
        throw std::runtime_error("handler failed");
    });
    dispatcher.appendListener(1, [](int n) { return n * 2; });
    dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Continue);
    dispatcher.setResultAggregator(1, [](const Dispatcher::ListenerResults & values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });
    auto report = dispatcher.dispatchWithResults(1, 3);
    // report.results == {6}; *report.aggregate == 6.
    // report.errors[0].handle identifies broken.
    try {
        std::rethrow_exception(report.errors[0].exception);
    }
    catch(const std::runtime_error &) {
        // Handle the failure; the other handler has already completed.
    }
}
```

`dispatchWithResults` and `directDispatchWithResults` remain restricted to ownable,
non-void returns. New `dispatchWithReport` and `directDispatchWithReport` use the same
dispatch path and also support `void` and non-ownable returns. For those prototypes,
`results` and `handles` stay empty, the aggregate is unset, and `errors` records failures.
All dispatch APIs preserve their existing event extraction/argument-passing conventions.

Ordinary `dispatch`/`directDispatch` also honor `Continue`, but discard values and errors.
Use a report API when errors must be inspected. `Propagate` still throws on the first
failure, stops invocation, skips aggregation and returns no partial report.

## Selection, cancellation and failure boundaries

The policy and selection configuration are captured together after mixin filters run.
Changes during ordering or a callback affect later or nested dispatches. Queue policy
is chosen at processing time, not enqueue time.

Ordering and plans keep their existing validation: omitted, duplicate, foreign, removed
and expired handles do not contribute additional values or errors. Replacement arguments
are used for the failing handler's call as well. A failed call does not roll back changes
to reference arguments or other application state. The continuation/cancellation policy
is checked after failed calls too, with the actual arguments; it can stop the remaining calls.

`Continue` catches exceptions from invoking a handler, including argument construction
performed by that call. Filter, ordering/planner, snapshot-copy, continuation-policy and
aggregator exceptions still propagate. Failures while storing a returned value, a handle
or an error record also propagate, and no partial report is returned. Error collection
can itself allocate; successful calls do not allocate error-vector storage.

## Queued reports

`enqueueWithResults` futures include the new errors list for ownable non-void returns.
`enqueueWithReport` returns the same `ResultFuture` for any prototype, including `void`:

```cpp
#include "eventpp/eventqueue.h"

eventpp::EventQueue<int, void()> queue;
queue.appendListener(1, [] { throw std::runtime_error("queued handler failed"); });
queue.setListenerExceptionPolicy(1, eventpp::ListenerExceptionPolicy::Continue);
auto future = queue.enqueueWithReport(1);
queue.process();
auto report = future.get(); // report.errors.size() == 1.
```

In `Continue` mode, handler failures are data in the completed report. With `Propagate`,
or with an infrastructure failure, `future.get()` rethrows the event's exception.
Ordinary `enqueue` discards collected errors in `Continue` mode. Clearing/destruction,
take/peek copies and exactly-once completion follow the same
[queued-result lifetime rules](queue_results.md). No work is processed merely by obtaining
or inspecting a future.

## Costs and regression check

The default propagation path still invokes an ordinary callback list directly. It
does not allocate a listener snapshot or error buffer. `Continue` adds exception
handling around each invocation; report errors allocate storage only on failures.
This check measured existing successful-call workloads with the default policy,
not the cost of throwing exceptions or contended metadata updates.

Compared with fork commit `17ff3a7`, a local MSVC x64 Release run on 2026-10-05 used
four alternating rounds, 20 samples per case and one logical CPU. Ordinary dispatch
medians showed no slowdown:

| Listeners | Before (ns/event) | After (ns/event) |
| --- | ---: | ---: |
| 1 | 79.565 | 76.752 |
| 8 | 304.438 | 303.131 |
| 32 | 1162.478 | 1052.903 |
| 128 | 4162.244 | 4114.221 |

Treat timing differences as local observations, not guaranteed speedups; even
unchanged `CallbackList` workloads varied. Allocation counts were unchanged for
all measured successful-call workloads. On this build, the empty errors vector
adds 24 bytes to `DispatchResult` and each queued-result future state. Combining
selection and exception configuration enlarged the map's allocated sentinel by
8 bytes per dispatcher, without enlarging callback nodes or dispatcher objects.
Configured selection/policy map entries also contain the added policy value.

The [summary CSV](benchmark_results/errors_metadata_summary.csv) and
[environment metadata](benchmark_results/errors_metadata_environment.json) record
the complete comparison. In these files, `upstream` labels the previous fork,
and `fork` labels the working headers; `baseline_has_fork_api` is true. Reproduce
with `tests/benchmark/overhead/run.py --baseline 17ff3a7 --baseline-features`.
