# Listener metadata and event ordering

`EventDispatcher` and `EventQueue` support application-defined selection and ordering
of listeners for each event. Existing registrations and dispatches keep their normal
list order unless an ordering function is set. The heterogeneous classes are unchanged.

## API

The public types are:

- `ListenerMetadata`: `Policies::ListenerMetadata`, or `std::map<std::string, std::string>` by default.
- `ListenerInfo`: a record with `Handle handle` and `ListenerMetadata metadata`.
- `ListenerList`: `std::vector<ListenerInfo>` in the original callback list order.
- `ListenerOrder`: `std::vector<Handle>` describing the listeners to invoke, in order.
- `ListenerOrdering`: `std::function<ListenerOrder(const ListenerList &, const T &...)>`.
  Each `T` is the corresponding callback argument type with references removed.

The dictionary type is chosen at compile time. For example, an application can use
`std::unordered_map<std::string, int>` or its own value type. C++11 has no `std::any`;
mixed dictionary values require an application-provided value wrapper. Metadata must
be default-constructible and copy-constructible.

```cpp
Handle appendListener(const Event &, const Callback &, const ListenerMetadata &);
Handle prependListener(const Event &, const Callback &, const ListenerMetadata &);
Handle insertListener(const Event &, const Callback &, const Handle & before,
                      const ListenerMetadata &);
void setListenerOrdering(const Event &, const ListenerOrdering &);
void clearListenerOrdering(const Event &);
```

The existing overloads without metadata remain available. Supplied metadata is copied
at registration and is not subsequently changed by the library. Registrations without
metadata do not allocate metadata storage; their snapshot records contain a default value.
The corresponding `CallbackList::append`, `prepend`, and `insert` overloads also accept
metadata, which is preserved when a callback list is copied.

Setting an empty ordering function is equivalent to clearing it. Ordering can be set
before listeners are registered. A returned empty order skips all listeners for that
dispatch without removing any subscriptions.

## C++11 example

```cpp
#include <eventpp/eventdispatcher.h>
#include <algorithm>
#include <map>
#include <string>

struct Policies {
    using ListenerMetadata = std::map<std::string, int>;
};
using Dispatcher = eventpp::EventDispatcher<int, void(int), Policies>;

void configure(Dispatcher & events) {
    events.appendListener(1, [](int value) { /* handle value */ },
                          {{"priority", 10}, {"minimum", 0}});
    events.appendListener(1, [](int value) { /* handle value */ },
                          {{"priority", 20}, {"minimum", 5}});

    events.setListenerOrdering(1,
        [](const Dispatcher::ListenerList & listeners, const int & value) {
            Dispatcher::ListenerList selected;
            for(const auto & listener : listeners) {
                if(value >= listener.metadata.at("minimum")) {
                    selected.push_back(listener);
                }
            }
            std::stable_sort(selected.begin(), selected.end(),
                [](const Dispatcher::ListenerInfo & a, const Dispatcher::ListenerInfo & b) {
                    return a.metadata.at("priority") > b.metadata.at("priority");
                });
            Dispatcher::ListenerOrder order;
            for(const auto & listener : selected) {
                order.push_back(listener.handle);
            }
            return order;
        });

    events.dispatch(1, 7); // Second listener, then first listener.
    events.dispatch(1, 3); // First listener only.
    events.clearListenerOrdering(1);
    events.dispatch(1, 3); // Both listeners, in registration order.
}
```

## Dispatch behavior

1. Existing mixin filters run first. If they reject the event, ordering is not called.
2. The dispatcher takes a snapshot of the event's listeners and their metadata.
3. The ordering function receives that snapshot and read-only references to the callback
   arguments. The separate event key is not an extra argument; if the key is already
   part of the callback signature, it is included normally. Arguments reflect any changes
   made by mixin filters. These references and the snapshot are valid during this call.
4. The selected listeners are invoked synchronously in the returned order. Callback
   arguments retain the original callback signature, including mutable references.
   The existing `canContinueInvoking` policy is checked after each invoked listener.

Only handles from the snapshot are accepted. Invalid, expired, foreign, newly added,
and repeated handles are ignored; each subscription can run at most once per dispatch.
The ordering function is not called for events which have no callback list yet.

Neither the ordering function nor the listeners run under internal mutexes. They can
add/remove subscriptions, change ordering, or dispatch nested events. New subscriptions
are absent from the current snapshot but participate in later and nested dispatches.
Removed subscriptions are skipped if removal precedes the invocation check. A callback
already starting in another thread is not interrupted by removal.

Changing/clearing an ordering function does not affect a dispatch that has already
captured it. Nested dispatches use the current configuration and build their own snapshot.
Concurrent dispatches may execute the same ordering callable simultaneously: mutable
state captured by application callables needs application synchronization.

Ordering and callback exceptions propagate to the caller. No listeners run if ordering
throws. The dispatcher remains usable, and ordinary listener exception behavior is unchanged.

`EventQueue` computes ordering at processing time, using the listeners and configuration
then present, rather than at enqueue time. `dispatch`, `process`, `processOne`, `processIf`,
and `processUntil` all use the same mechanism.

Copying a dispatcher copies its ordering callable and listener metadata, with new listener
handles. Functions should choose handles from their supplied snapshot rather than capture
handles from the original dispatcher. Moving/swapping transfers the configuration with
the listeners.

## Cost

Events without ordering use the original callback traversal, without snapshot allocations
or metadata copies. Each callback node has an additional shared pointer for optional
metadata. With ordering enabled, snapshot creation copies each listener's metadata, and
handle validation takes `O(N log N + M log N)` time for `N` snapshot listeners and `M`
returned handles, plus the application ordering function and callbacks.
