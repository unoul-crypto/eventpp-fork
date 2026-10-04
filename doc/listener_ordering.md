# Listener metadata and event ordering

Selection and argument plans also work with [handler result collection](return_results.md).

`EventDispatcher` and `EventQueue` support application-defined selection and ordering
of listeners for each event. Existing registrations and dispatches keep their normal
list order unless an ordering function or planner is set. The heterogeneous classes are unchanged.

## API

The public types are:

- `ListenerMetadata`: `Policies::ListenerMetadata`, or `std::map<std::string, std::string>` by default.
- `ListenerInfo`: a record with `Handle handle` and `ListenerMetadata metadata`.
- `ListenerList`: `std::vector<ListenerInfo>` in the original callback list order.
- `ListenerOrder`: `std::vector<Handle>` describing the listeners to invoke, in order.
- `ListenerOrdering`: `std::function<ListenerOrder(const ListenerList &, const T &...)>`.
  Each `T` is the corresponding callback argument type with references removed.
- `ListenerPlan`: a move-only plan containing handles and optional owned replacement arguments.
- `ListenerPlanner`: `std::function<ListenerPlan(const ListenerList &, const T &...)>`.

The dictionary type is chosen at compile time. For example, an application can use
`std::unordered_map<std::string, int>` or its own value type. C++11 has no `std::any`;
mixed dictionary values require an application-provided value wrapper. Metadata must
be default-constructible and copy-constructible.

```cpp
Handle appendListener(const Event &, const Callback &, const ListenerMetadata &);
Handle prependListener(const Event &, const Callback &, const ListenerMetadata &);
Handle insertListener(const Event &, const Callback &, const Handle & before,
                      const ListenerMetadata &);
bool setListenerMetadata(const Event &, const Handle &, const ListenerMetadata &);
bool getListenerMetadata(const Event &, const Handle &, ListenerMetadata & output) const;
ListenerList getListeners(const Event &) const;
void setListenerOrdering(const Event &, const ListenerOrdering &);
void clearListenerOrdering(const Event &);
void setListenerPlanner(const Event &, const ListenerPlanner &);
void clearListenerPlanner(const Event &);
```

The existing overloads without metadata remain available. Supplied metadata is copied
at registration and can be replaced explicitly with `setListenerMetadata`. Registrations without
metadata do not allocate metadata storage; their snapshot records contain a default value.
The corresponding `CallbackList::append`, `prepend`, and `insert` overloads also accept
metadata, which is preserved when a callback list is copied.

`setListenerMetadata(event, handle, metadata)` replaces the complete metadata value
without changing the subscription's handle or its list position. It returns false for
an invalid, removed, expired or foreign handle, or a handle belonging to a different event.
Copying the new metadata can throw; the previous value remains intact on copy failure.
Standalone `CallbackList` provides `setListenerMetadata(handle, metadata)`.

`getListenerMetadata(event, handle, output)` copies the current metadata into `output`.
It returns false for the same invalid handles as the setter or for a missing event,
leaving `output` unchanged. A listener registered without metadata returns a
default-constructed metadata value and true. The returned value is independent of
the stored metadata; changing it does not update the listener. Use the setter to save
changes. `EventQueue` inherits this method, and standalone `CallbackList` provides
`getListenerMetadata(handle, output) const`.

```cpp
Dispatcher::ListenerMetadata metadata;
if(dispatcher.getListenerMetadata(event, handle, metadata)) {
    metadata["priority"] = "10"; // With the default dictionary type.
    dispatcher.setListenerMetadata(event, handle, metadata);
}
```

Reading captures the stored value under the list mutex, then assigns it to `output`
outside the mutex. Concurrent replacement or removal does not invalidate that copy.
The getter requires copy-assignable metadata; construction or assignment exceptions
propagate without changing the stored value. If assignment throws, the state of
`output` follows the metadata type's assignment guarantee. Reading and then updating
is two separate operations, so concurrent updates may overwrite one another.

Replacement is synchronized with snapshot creation. A selection function continues to
see its original snapshot even if it updates metadata during its execution. Later and
nested dispatches see the replacement. Metadata updates on one copy of a dispatcher do
not affect another copy. Pointer values inside application metadata retain ordinary shared
ownership semantics; replacement is not a deep copy of pointed-to objects.
The getter reads the current stored value even when called from a selection function
whose listener snapshot still contains an older value.

`getListeners(event)` returns a snapshot of all currently registered listeners for that
event. Each `ListenerInfo` contains its `handle` and a copy of its `metadata`. The order
is the callback list order, including the effects of `prependListener` and `insertListener`.
A missing event or an empty list returns an empty vector. `EventQueue` inherits this
const method; inspecting it does not process queued events.

```cpp
auto listeners = dispatcher.getListeners(event);
for(const auto & listener : listeners) {
    // Read listener.metadata; use listener.handle to update or remove the subscription.
}
```

Inspection does not run filters, ordering/planning functions, callbacks or aggregators,
and includes listeners that a selection function might omit on a later dispatch. The
snapshot is copied under the callback list mutex. Later subscription or metadata changes
do not update the returned vector; modifying its records does not update subscriptions.
Handles are weak: a snapshot does not extend callback lifetime, and a returned handle
may already be expired or removed when the caller uses it. As with other metadata copies,
pointer members retain their ordinary sharing semantics. Metadata-copy and allocation
exceptions propagate without changing the subscriptions.

Setting an empty ordering function is equivalent to clearing it. Ordering can be set
before listeners are registered. A returned empty order skips all listeners for that
dispatch without removing any subscriptions.

An event has one active selection function. `setListenerPlanner` replaces its ordering
function, and `setListenerOrdering` replaces its planner. Both clear methods (and setting
an empty function of either kind) restore normal list traversal.

## Individual arguments with ListenerPlan

A planner can select listeners, order them, and prepare a different complete argument
list for each one. Its input snapshot and read-only event arguments are the same as for
an ordering function. The existing `ListenerOrder` API remains available.

```cpp
using Dispatcher = eventpp::EventDispatcher<int, void(int, const std::string &)>;

void configurePlan(Dispatcher & events) {
    events.appendListener(1, [](int value, const std::string & text) { /* first */ });
    events.appendListener(1, [](int value, const std::string & text) { /* second */ });

    events.setListenerPlanner(1,
        [](const Dispatcher::ListenerList & listeners,
           const int & value, const std::string & text) {
            Dispatcher::ListenerPlan plan;
            plan.add(listeners[1].handle, value * 2, text + "!");
            plan.add(listeners[0].handle);
            return plan;
        });

    events.dispatch(1, 7, "hello");
    // Second listener gets (14, "hello!"), then first gets (7, "hello").
}
```

`plan.add(handle)` uses the original arguments without storing copies. Their reference
semantics are preserved: changes made by an earlier callback to a shared original `T&`
are visible to later callbacks that also use the original arguments. Non-copyable and
abstract reference arguments work with this form.

`plan.add(handle, values...)` stores its own `std::tuple<std::decay<Args>::type...>` for
that invocation. Supply all arguments, with types convertible to the callback signature.
Values are copied from lvalues or moved from rvalues when added. The planner's locals
can then be destroyed safely; replacement values live until the plan is destroyed after
dispatch, including when an exception unwinds it. Move-only replacements are supported
when the callback signature can consume them, for example `std::unique_ptr<T>&`.

A callback taking `T&` receives a reference to the stored replacement, rather than the
original object. Changes do not affect other invocation tuples or the original argument.
This is ordinary value ownership, not a deep copy: pointer and shared-pointer values may
still refer to the same external object. Referenced replacement values must not be retained
past dispatch. Replacement argument types must be storable as values; an abstract type
cannot be stored this way.

The plan provides `size()` and `empty()`. It can be moved and returned from a planner,
but cannot be copied, ensuring its owned argument tuples are not shared between dispatches.
For a callback without arguments, use `plan.add(handle)`.

Omitting a handle skips that listener for this dispatch. Duplicate and invalid handles
follow the same rules as `ListenerOrder`; only the first occurrence is considered.
The `canContinueInvoking` policy receives the actual arguments used for each callback,
including mutations to replacements. Returning false stops the entire dispatch, even if
the original event object was not changed.

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
3. The ordering function or planner receives that snapshot and read-only references to the callback
   arguments. The separate event key is not an extra argument; if the key is already
   part of the callback signature, it is included normally. Arguments reflect any changes
   made by mixin filters. These references and the snapshot are valid during this call.
4. The selected listeners are invoked synchronously in the returned order or plan. Callback
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

Copying a dispatcher copies its ordering/planner callable and listener metadata, with new listener
handles. Functions should choose handles from their supplied snapshot rather than capture
handles from the original dispatcher. Moving/swapping transfers the configuration with
the listeners.

## Cost

Events without ordering use the original callback traversal, without snapshot allocations
or metadata copies. Each callback node has an additional shared pointer for optional
metadata. With ordering enabled, snapshot creation copies each listener's metadata, and
handle validation takes `O(N log N + M log N)` time for `N` snapshot listeners and `M`
returned handles, plus the application ordering function and callbacks.
The snapshot vector reserves space once to avoid repeated metadata copies during growth.
Validation uses one sorted array of weak owners and invocation flags rather than one
tree allocation per listener. Owner identity is preserved even for expired handles;
validation storage is local to each dispatch. An empty selection skips validation allocation.
A plan has the same validation cost. Entries using original arguments allocate no argument
storage; each entry with replacements allocates owned storage for its tuple. Planner support
adds no fields to callback nodes and does not create plans when ordinary ordering is used.
Updating metadata copies the supplied value and checks handle membership in `O(N)` time.
Reading metadata also checks membership in `O(N)` time, followed by one assignment.
`getListeners` traverses the list in `O(N)` time and allocates its snapshot vector plus
any storage needed to copy metadata. It uses the same reserved snapshot builder as selection.
