// Standalone, single-threaded benchmark. Both versions use the default threading policy.
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <new>
#include <numeric>
#include <stdexcept>
#include <vector>

#ifdef OVERHEAD_ALLOCATIONS
namespace allocation {
// Count requested C++ heap bytes, excluding this header and allocator bookkeeping.
struct alignas(std::max_align_t) Header {
    std::size_t bytes;
    std::size_t generation;
};
bool enabled = false;
std::size_t generation = 0, calls = 0, bytes = 0, live = 0, peak = 0;
void begin() {
    ++generation;
    calls = bytes = live = peak = 0;
    enabled = true;
}
void end() { enabled = false; }
}

void * operator new(std::size_t size)
{
    if(size == 0) { size = 1; }
    if(size > static_cast<std::size_t>(-1) - sizeof(allocation::Header)) { throw std::bad_alloc(); }
    auto header = static_cast<allocation::Header *>(std::malloc(sizeof(allocation::Header) + size));
    if(! header) { throw std::bad_alloc(); }
    header->bytes = size;
    header->generation = allocation::enabled ? allocation::generation : 0;
    if(allocation::enabled) {
        ++allocation::calls;
        allocation::bytes += size;
        allocation::live += size;
        allocation::peak = (std::max)(allocation::peak, allocation::live);
    }
    return header + 1;
}
void operator delete(void * pointer) noexcept
{
    if(! pointer) { return; }
    auto header = static_cast<allocation::Header *>(pointer) - 1;
    if(header->generation != 0 && header->generation == allocation::generation) {
        allocation::live -= header->bytes;
    }
    std::free(header);
}
void * operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void * pointer) noexcept { ::operator delete(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void * pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void * pointer, std::size_t) noexcept { ::operator delete(pointer); }
#endif
#endif

namespace {
using Dispatcher = eventpp::EventDispatcher<int, int(int)>;
using Queue = eventpp::EventQueue<int, int(int)>;
using List = eventpp::CallbackList<int(int)>;
std::uint64_t checksum = 0;
const int queueBatch = 64;
#ifdef OVERHEAD_FORK
struct CompactMetadata { int priority; int factor; };
struct CompactPolicies { using ListenerMetadata = CompactMetadata; };
#endif

// An observable callback common to both builds; prevents empty-loop elimination.
int callback(int value)
{
    checksum += static_cast<unsigned int>(value);
    return value + 1;
}

void reportCase(const char * name, int listeners, const std::function<void()> & operation,
    int operationsPerIteration = 1)
{
    // Warm listener traversal and the queue's recycled list nodes before measuring.
    for(int i = 0; i < 64; ++i) { operation(); }
#ifdef OVERHEAD_ALLOCATIONS
    const int iterations = 64;
    allocation::begin();
    for(int i = 0; i < iterations; ++i) { operation(); }
    allocation::end();
    const double count = static_cast<double>(iterations * operationsPerIteration);
    std::cout << "steady," << name << ',' << listeners << ",0," << count << ",,"
        << allocation::calls / count << ',' << allocation::bytes / count << ',' << allocation::peak << '\n';
#else
    using Clock = std::chrono::steady_clock;
    int iterations = 64;
    for(;;) {
        auto start = Clock::now();
        for(int i = 0; i < iterations; ++i) { operation(); }
        if(Clock::now() - start >= std::chrono::milliseconds(10) || iterations >= (1 << 22)) { break; }
        iterations *= 2;
    }
    double samples[5];
    for(double & sample : samples) {
        auto start = Clock::now();
        for(int i = 0; i < iterations; ++i) { operation(); }
        sample = std::chrono::duration<double, std::nano>(Clock::now() - start).count()
            / (static_cast<double>(iterations) * operationsPerIteration);
    }
    for(int i = 0; i < 5; ++i) {
        std::cout << "timing," << name << ',' << listeners << ',' << i << ','
            << iterations * operationsPerIteration << ',' << samples[i] << ",,,\n";
    }
#endif
}

#ifdef OVERHEAD_ALLOCATIONS
void reportSetup(const char * name, int listeners, const std::function<void()> & setup)
{
    allocation::begin();
    setup();
    allocation::end();
    std::cout << "setup," << name << ',' << listeners << ",0,1,,"
        << allocation::calls << ',' << allocation::bytes << ',' << allocation::peak << '\n';
}
template <typename T>
void reportSize(const char * name)
{
    std::cout << "size," << name << ",0,0,1,,," << sizeof(T) << ",\n";
}

void registration(int count)
{
    reportSetup("register_list", count, [=] {
        List list;
        for(int i = 0; i < count; ++i) { list.append(callback); }
    });
    reportSetup("register_dispatcher", count, [=] {
        Dispatcher dispatcher;
        for(int i = 0; i < count; ++i) { dispatcher.appendListener(1, callback); }
    });
    reportSetup("register_queue", count, [=] {
        Queue queue;
        for(int i = 0; i < count; ++i) { queue.appendListener(1, callback); }
    });
    reportSetup("pending_queue_64", count, [=] {
        Queue queue;
        for(int i = 0; i < count; ++i) { queue.appendListener(1, callback); }
        for(int i = 0; i < queueBatch; ++i) { queue.enqueue(1, 7); }
    });
#ifdef OVERHEAD_FORK
    reportSetup("register_metadata", count, [=] {
        Dispatcher dispatcher;
        const Dispatcher::ListenerMetadata metadata {{"priority", "1"}, {"group", "main"}};
        for(int i = 0; i < count; ++i) { dispatcher.appendListener(1, callback, metadata); }
    });
    reportSetup("queue_results_64_lifecycle", count, [=] {
        Queue queue;
        for(int i = 0; i < count; ++i) { queue.appendListener(1, callback); }
        std::vector<Queue::ResultFuture> futures;
        futures.reserve(queueBatch);
        for(int i = 0; i < queueBatch; ++i) { futures.push_back(queue.enqueueWithResults(1, 7)); }
        // Completion avoids measuring exception allocations from abandoned promises.
        queue.process();
        for(auto & future : futures) { future.get(); }
    });
#endif
}
#endif

void benchmark(int count)
{
    List list;
    Dispatcher dispatcher;
    Queue queue;
    for(int i = 0; i < count; ++i) {
        list.append(callback);
        dispatcher.appendListener(1, callback);
        queue.appendListener(1, callback);
    }
    reportCase("callback_list", count, [&] { list(7); });
    reportCase("dispatch", count, [&] { dispatcher.dispatch(1, 7); });
    reportCase("enqueue_process", count, [&] {
        for(int i = 0; i < queueBatch; ++i) { queue.enqueue(1, 7); }
        queue.process();
    }, queueBatch);

#ifdef OVERHEAD_FORK
    Dispatcher withMetadata;
    const Dispatcher::ListenerMetadata metadata {{"priority", "1"}, {"group", "main"}};
    Dispatcher::Handle last;
    for(int i = 0; i < count; ++i) {
        auto handle = withMetadata.appendListener(1, callback, metadata);
        last = handle;
    }
    reportCase("dispatch_metadata", count, [&] { withMetadata.dispatch(1, 7); });
    Dispatcher::ListenerMetadata output;
    reportCase("get_metadata", count, [&] {
        if(! withMetadata.getListenerMetadata(1, last, output)) { throw std::runtime_error("missing listener"); }
    });
    reportCase("set_metadata", count, [&] {
        if(! withMetadata.setListenerMetadata(1, last, metadata)) { throw std::runtime_error("missing listener"); }
    });
    auto ordering = [](const Dispatcher::ListenerList & listeners, const int &) {
        Dispatcher::ListenerOrder order;
        order.reserve(listeners.size());
        for(const auto & listener : listeners) { order.push_back(listener.handle); }
        return order;
    };
    dispatcher.setListenerOrdering(1, ordering);
    reportCase("ordering", count, [&] { dispatcher.dispatch(1, 7); });
    dispatcher.clearListenerOrdering(1);
    withMetadata.setListenerOrdering(1, ordering);
    reportCase("ordering_metadata", count, [&] { withMetadata.dispatch(1, 7); });
    withMetadata.clearListenerOrdering(1);

    using CompactDispatcher = eventpp::EventDispatcher<int, int(int), CompactPolicies>;
    CompactDispatcher compact;
    for(int i = 0; i < count; ++i) { compact.appendListener(1, callback, CompactMetadata {1, 2}); }
    compact.setListenerOrdering(1, [](const CompactDispatcher::ListenerList & listeners, const int &) {
        CompactDispatcher::ListenerOrder order;
        order.reserve(listeners.size());
        for(const auto & listener : listeners) { order.push_back(listener.handle); }
        return order;
    });
    reportCase("ordering_compact_metadata", count, [&] { compact.dispatch(1, 7); });

    dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int &) {
        Dispatcher::ListenerPlan plan;
        for(const auto & listener : listeners) { plan.add(listener.handle); }
        return plan;
    });
    reportCase("plan_original", count, [&] { dispatcher.dispatch(1, 7); });
    dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int & value) {
        Dispatcher::ListenerPlan plan;
        for(const auto & listener : listeners) { plan.add(listener.handle, value + 1); }
        return plan;
    });
    reportCase("plan_arguments", count, [&] { dispatcher.dispatch(1, 7); });
    auto planned = dispatcher.dispatchWithResults(1, 7);
    if(planned.results != std::vector<int>(count, 9)) { throw std::runtime_error("incorrect planned results"); }
    reportCase("plan_results", count, [&] { dispatcher.dispatchWithResults(1, 7); });
    dispatcher.clearListenerPlanner(1);

    auto ordinary = dispatcher.dispatchWithResults(1, 7);
    if(ordinary.results != std::vector<int>(count, 8)) { throw std::runtime_error("incorrect results"); }
    reportCase("results", count, [&] { dispatcher.dispatchWithResults(1, 7); });
    reportCase("results_metadata", count, [&] { withMetadata.dispatchWithResults(1, 7); });
    dispatcher.setResultAggregator(1, [](const Dispatcher::ListenerResults & values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });
    auto aggregated = dispatcher.dispatchWithResults(1, 7);
    if(! aggregated.aggregate || *aggregated.aggregate != count * 8) { throw std::runtime_error("incorrect aggregate"); }
    reportCase("results_aggregate", count, [&] { dispatcher.dispatchWithResults(1, 7); });

    std::vector<Queue::ResultFuture> futures;
    futures.reserve(queueBatch);
    reportCase("enqueue_results_process", count, [&] {
        for(int i = 0; i < queueBatch; ++i) { futures.push_back(queue.enqueueWithResults(1, 7)); }
        queue.process();
        for(auto & future : futures) { future.get(); }
        futures.clear();
    }, queueBatch);
#endif
}
}

int main()
{
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "kind,scenario,listeners,sample,operations,ns_per_op,allocations_per_op,bytes_per_op,peak_bytes\n";
#if defined(_MSC_FULL_VER)
    std::cerr << "compiler=MSVC " << _MSC_FULL_VER << '\n';
#elif defined(__VERSION__)
    std::cerr << "compiler=" << __VERSION__ << '\n';
#endif
#ifdef OVERHEAD_ALLOCATIONS
    reportSize<List>("callback_list_object");
    reportSize<Dispatcher>("dispatcher_object");
    reportSize<Queue>("queue_object");
    reportSize<Queue::QueuedEvent>("queued_event");
    List sizingList;
    auto sizingHandle = sizingList.append(callback);
    std::cout << "size,callback_node,0,0,1,,," << sizeof(*sizingHandle.lock()) << ",\n";
#endif
    for(int count : {1, 8, 32, 128}) {
#ifdef OVERHEAD_ALLOCATIONS
        registration(count);
#endif
        benchmark(count);
    }
    std::cerr << "checksum=" << checksum << '\n';
}
