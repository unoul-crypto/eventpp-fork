# Introduction to eventpp library

eventpp includes three core classes, CallbackList, EventDispatcher, and EventQueue. Each class has a different purpose and usage.  

## Class CallbackList

CallbackList is the fundamental class in eventpp. The other classes EventDispatcher and EventQueue are built on CallbackList.  

CallbackList holds a list of callbacks. When a CallbackList is being invoked, CallbackList simply invokes each callback one by one. Consider CallbackList as the signal/slot system in Qt, or the callback function pointer in some Windows APIs (such as lpCompletionRoutine in `ReadFileEx`).  
The *callback* can be any callback target -- functions, pointers to functions, , pointers to member functions, lambda expressions, and function objects.  

CallbackList equivalents the 'signal' in other event system such as Qt. There is no 'slot' (or callback) equivalence in eventpp. Any callable can be the slot (or to say, callback).

CallbackList is ideal when there are very few kinds of events. Each event can have its own CallbackList, and each CallbackList can have a different prototype. For example,
```c++
eventpp::CallbackList<void()> onStart;
eventpp::CallbackList<void(MyStopReason)> onStop;
```
However, if there are many kinds of events, hundreds of to unlimited (this is quite common in GUI or game systems), it would be crazy to use CallbackList for each event. This is how EventDispatcher comes useful.  

## Class EventDispatcher

EventDispatcher is something like std::map<EventType, CallbackList>.

EventDispatcher holds a map of `<EventType, CallbackList>` pairs. On dispatching, EventDispatcher finds the CallbackList of the event type, then invoke the callback list. The invocation is always synchronous. The listeners are triggered when `EventDispatcher::dispatch` is called.  

EventDispatcher is ideal when there are many kinds of events, or the number of events cannot be predetermined. Each event is distinguished by the event type. For example,
```c++
enum class MyEventType
{
    redraw,
    mouseDown,
    mouseUp,
    //... maybe 200 other events here
};

struct MyEvent {
    MyEventType type;
    // data that all events may need
};

struct MyEventPolicies
{
    static MyEventType getEvent(const MyEvent & e) {
        return e.type;
    }
};

eventpp::EventDispatcher<MyEventType, void(const MyEvent &), MyEventPolicies> dispatcher;
dispatcher.dispatch(MyEvent { MyEventType::redraw });
```
(Note: if you are confused with MyEventPolicies in above sample, please read the [document of policies](policies.md), and just consider the dispatcher as `eventpp::EventDispatcher<MyEventType, void(const MyEvent &)> dispatcher` for now.)  
The disadvantage of EventDispatcher is that all events must have the same callback prototype (`void(const MyEvent &)` in the sample code). The common solution is that the callback takes a base class of Event and all events derive their own event data from Event. In the sample code, MyEvent is the base event class, the callback takes an argument of `const MyEvent &`.  

## Class EventQueue

EventQueue includes all features of EventDispatcher and adds event queue features. Note: EventQueue doesn't inherit from EventDispatcher, don't try to cast EventQueue to EventDispatcher.  
EventQueue is asynchronous. Events are cached in the queue when `EventQueue::enqueue` is called, and dispatched later when `EventQueue::process` is called.  
EventQueue is equivalent to the event system (QEvent) in Qt, or the message processing in Windows API.  

```c++
eventpp::EventQueue<int, void (const std::string &, const bool)> queue;

// Enqueue the events, the first argument is always the event type.
// The listeners are not triggered during enqueue.
queue.enqueue(3, "Hello", true);
queue.enqueue(5, "World", false);

// Process the event queue, dispatch all queued events.
queue.process();
```

## Thread safety
The default `MultipleThreading` policy synchronizes listener registration/removal,
traversal, metadata access, configuration and queue operations. `SingleThreading`
disables synchronization and requires calls from a single thread or external
locking. Copy, move, assignment, swap and destruction require exclusive access to
the affected objects; they must not overlap other operations on those objects.

Handlers, ordering/planning functions, aggregators and metadata updaters execute
outside internal locks. Concurrent dispatches can call the same function at the
same time; user code must synchronize any shared mutable state, including objects
referenced by event arguments. Metadata and configuration snapshots are local to
each dispatch. `updateListenerMetadata` may retry the updater after a conflicting
update, so the updater must tolerate repeated calls.

Operations from different threads have no guaranteed global order. Removing a
listener does not wait for a handler already selected for invocation to finish.
`empty()` and `emptyQueue()` are synchronized observations, not promises about
subsequent operations. `emptyQueue()` also accounts for batches currently being
processed. Queued reports/results share an atomic completion claim, so processing,
peek-copy dispatch and cancellation compete for one completion per queued event.


## Exception safety

All classes don't throw any exceptions.  
Exceptions may be thrown by underlying code when,  
1. Out of memory, new memory can't be allocated.  
2. The listeners (callbacks) throw exceptions during copying, moving, comparing, or invoking.

Almost all operations guarantee strong exception safety, which means the underlying data remains original value on exception is thrown.  
An except is `EventQueue::process`, on exception, the remaining events will not be dispatched, and the queue becomes empty.
