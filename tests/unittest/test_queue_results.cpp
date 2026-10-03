#include "test.h"
#include "eventpp/eventqueue.h"
#include "eventpp/utilities/orderedqueuelist.h"

#include <future>
#include <thread>
#include <stdexcept>
#include <numeric>

namespace {
using Queue = eventpp::EventQueue<int, int(int)>;

template <typename Future>
bool ready(Future & future)
{
	return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

template <typename Future>
bool brokenPromise(Future & future)
{
	try { future.get(); }
	catch(const std::future_error & error) { return error.code() == std::make_error_code(std::future_errc::broken_promise); }
	return false;
}

struct MetadataPolicies {
	using ListenerMetadata = std::map<std::string, int>;
};
struct Event {
	int type;
	int value;
};
struct EventPolicies {
	static int getEvent(const Event & event) { return event.type; }
};
struct OrderedPolicies {
	template <typename T> using QueueList = eventpp::OrderedQueueList<T>;
};
struct SinglePolicies {
	using Threading = eventpp::SingleThreading;
};

template <typename T>
struct HasResultEnqueue {
	template <typename U> static auto test(int) -> decltype(std::declval<U>().enqueueWithResults(1), std::true_type());
	template <typename U> static std::false_type test(...);
	static constexpr bool value = decltype(test<T>(0))::value;
};
}

TEST_CASE("EventQueue results, delayed completion with returns, handles and aggregate")
{
	Queue queue;
	int calls = 0;
	queue.appendListener(1, [&](int n) { ++calls; return n + 1; });
	queue.appendListener(1, [&](int n) { ++calls; return n * 2; });
	queue.setResultAggregator(1, [](const std::vector<int> & values) { return std::accumulate(values.begin(), values.end(), 0); });
	auto future = queue.enqueueWithResults(1, 5);
	REQUIRE_FALSE(ready(future));
	REQUIRE(calls == 0);
	REQUIRE(queue.process());
	REQUIRE(ready(future));
	auto result = future.get();
	REQUIRE(result.results == std::vector<int> {6, 10});
	REQUIRE(result.handles.size() == 2);
	REQUIRE(*result.aggregate == 16);
}

TEST_CASE("EventQueue results, each queued event has an independent completion")
{
	Queue queue;
	queue.appendListener(1, [](int n) { return n; });
	auto first = queue.enqueueWithResults(1, 1);
	queue.enqueue(1, 99);
	auto second = queue.enqueueWithResults(1, 2);
	REQUIRE(queue.processOne());
	REQUIRE(ready(first));
	REQUIRE_FALSE(ready(second));
	REQUIRE(first.get().results == std::vector<int> {1});
	REQUIRE(queue.process());
	REQUIRE(second.get().results == std::vector<int> {2});
}

TEST_CASE("EventQueue results, metadata, plans and aggregator are chosen at processing time")
{
	using EQ = eventpp::EventQueue<int, int(int), MetadataPolicies>;
	EQ queue;
	auto handle = queue.appendListener(1, [](int n) { return n; }, {{"factor", 1}});
	auto future = queue.enqueueWithResults(1, 3);
	REQUIRE(queue.setListenerMetadata(1, handle, {{"factor", 4}}));
	queue.setListenerPlanner(1, [](const EQ::ListenerList & listeners, const int & n) {
		EQ::ListenerPlan plan;
		plan.add(listeners[0].handle, n * listeners[0].metadata.at("factor"));
		return plan;
	});
	queue.setResultAggregator(1, [](const std::vector<int> & values) { return values[0] + 1; });
	REQUIRE(queue.process());
	auto result = future.get();
	REQUIRE(result.results == std::vector<int> {12});
	REQUIRE(*result.aggregate == 13);
}

TEST_CASE("EventQueue results, filtered processing leaves deferred futures pending")
{
	Queue queue;
	queue.appendListener(1, [](int n) { return n; });
	auto one = queue.enqueueWithResults(1, 1);
	auto two = queue.enqueueWithResults(1, 2);
	auto three = queue.enqueueWithResults(1, 3);
	REQUIRE(queue.processIf([](int n) { return n == 2; }));
	REQUIRE_FALSE(ready(one));
	REQUIRE(ready(two));
	REQUIRE_FALSE(ready(three));
	REQUIRE(queue.processUntil([](int n) { return n == 3; }));
	REQUIRE(one.get().results == std::vector<int> {1});
	REQUIRE(two.get().results == std::vector<int> {2});
	REQUIRE_FALSE(ready(three));
	REQUIRE(queue.process());
	REQUIRE(three.get().results == std::vector<int> {3});
}

TEST_CASE("EventQueue results, callback and aggregator exceptions are delivered through futures")
{
	Queue queue;
	queue.appendListener(1, [](int n) { if(n == 1) { throw std::runtime_error("handler failed"); } return n; });
	queue.setResultAggregator(1, [](const std::vector<int> & values) {
		if(values[0] == 2) { throw std::logic_error("aggregator failed"); }
		return values[0];
	});
	auto handlerFailure = queue.enqueueWithResults(1, 1);
	auto aggregatorFailure = queue.enqueueWithResults(1, 2);
	auto success = queue.enqueueWithResults(1, 3);
	REQUIRE_NOTHROW(queue.process());
	REQUIRE_THROWS_AS(handlerFailure.get(), std::runtime_error);
	REQUIRE_THROWS_AS(aggregatorFailure.get(), std::logic_error);
	REQUIRE(*success.get().aggregate == 3);
	queue.enqueue(1, 1);
	REQUIRE_THROWS_AS(queue.process(), std::runtime_error);
}

TEST_CASE("EventQueue results, clear and destruction cancel pending completions")
{
	Queue queue;
	auto cleared = queue.enqueueWithResults(1, 1);
	Queue::QueuedEvent copy;
	REQUIRE(queue.peekEvent(&copy));
	queue.clearEvents();
	REQUIRE(ready(cleared));
	REQUIRE(brokenPromise(cleared));
	queue.dispatch(copy); // A canceled peek copy must not restart work.

	Queue::ResultFuture destroyed;
	Queue::QueuedEvent peek;
	{
		Queue temporary;
		destroyed = temporary.enqueueWithResults(1, 2);
		REQUIRE(temporary.peekEvent(&peek));
	}
	REQUIRE(ready(destroyed));
	REQUIRE(brokenPromise(destroyed));
}

TEST_CASE("EventQueue results, taking transfers work and peek dispatch completes only once")
{
	Queue queue;
	int calls = 0;
	queue.appendListener(1, [&](int n) { ++calls; return n; });
	auto taken = queue.enqueueWithResults(1, 7);
	Queue::QueuedEvent event;
	REQUIRE(queue.takeEvent(&event));
	REQUIRE(queue.emptyQueue());
	queue.clearEvents();
	REQUIRE_FALSE(ready(taken));
	queue.dispatch(event);
	REQUIRE(taken.get().results == std::vector<int> {7});
	queue.dispatch(event);
	REQUIRE(calls == 1);
	auto peeked = queue.enqueueWithResults(1, 8);
	REQUIRE(queue.peekEvent(&event));
	const auto & constEvent = event;
	queue.dispatch(constEvent);
	REQUIRE(queue.process());
	REQUIRE(peeked.get().results == std::vector<int> {8});
	REQUIRE(calls == 2);
}

TEST_CASE("EventQueue results, abandoning taken work produces broken promise")
{
	Queue queue;
	auto future = queue.enqueueWithResults(1, 7);
	{
		Queue::QueuedEvent event;
		REQUIRE(queue.takeEvent(&event));
		REQUIRE_FALSE(ready(future));
	}
	REQUIRE(ready(future));
	REQUIRE(brokenPromise(future));
}

TEST_CASE("EventQueue results, custom event extraction and empty listener vectors")
{
	using EQ = eventpp::EventQueue<int, int(const Event &), EventPolicies>;
	EQ queue;
	queue.appendListener(3, [](const Event & event) { return event.value; });
	auto future = queue.enqueueWithResults(Event {3, 42});
	queue.process();
	REQUIRE(future.get().results == std::vector<int> {42});
	auto empty = queue.enqueueWithResults(Event {4, 0});
	queue.process();
	REQUIRE(empty.get().results.empty());
	static_assert(! HasResultEnqueue<eventpp::EventQueue<int, void()> >::value, "Void callbacks cannot enqueue with values.");
}

TEST_CASE("EventQueue results, move-only arguments and mutable taken-event dispatch")
{
	using EQ = eventpp::EventQueue<int, int(std::unique_ptr<int> &)>;
	EQ queue;
	queue.appendListener(1, [](std::unique_ptr<int> & n) { int value = *n; n.reset(); return value; });
	auto first = queue.enqueueWithResults(1, std::unique_ptr<int>(new int(7)));
	queue.process();
	REQUIRE(first.get().results == std::vector<int> {7});
	auto second = queue.enqueueWithResults(1, std::unique_ptr<int>(new int(8)));
	EQ::QueuedEvent event;
	REQUIRE(queue.takeEvent(&event));
	queue.dispatch(event);
	REQUIRE(second.get().results == std::vector<int> {8});
}

TEST_CASE("EventQueue results, custom queue container and threading policy")
{
	using EQ = eventpp::EventQueue<int, int(), OrderedPolicies>;
	EQ queue;
	std::vector<int> order;
	queue.appendListener(1, [&] { order.push_back(1); return 1; });
	queue.appendListener(2, [&] { order.push_back(2); return 2; });
	auto second = queue.enqueueWithResults(2);
	auto first = queue.enqueueWithResults(1);
	queue.process();
	REQUIRE(order == std::vector<int> {1, 2});
	REQUIRE(first.get().results == std::vector<int> {1});
	REQUIRE(second.get().results == std::vector<int> {2});
	eventpp::EventQueue<int, int(), SinglePolicies> single;
	single.appendListener(1, [] { return 42; });
	auto future = single.enqueueWithResults(1);
	single.process();
	REQUIRE(future.get().results == std::vector<int> {42});
}

TEST_CASE("EventQueue results, copied queues do not duplicate pending futures")
{
	Queue queue;
	queue.appendListener(1, [](int n) { return n; });
	auto original = queue.enqueueWithResults(1, 1);
	Queue copied(queue);
	REQUIRE_FALSE(copied.process());
	REQUIRE_FALSE(ready(original));
	auto fromCopy = copied.enqueueWithResults(1, 2);
	copied.process();
	REQUIRE(fromCopy.get().results == std::vector<int> {2});
	queue.process();
	REQUIRE(original.get().results == std::vector<int> {1});
}

TEST_CASE("EventQueue results, ordinary failure cancels unprocessed batch futures")
{
	using EQ = eventpp::EventQueue<int, int(int), OrderedPolicies>;
	EQ queue;
	queue.appendListener(0, [](int) -> int { throw std::runtime_error("ordinary failure"); });
	int calls = 0;
	queue.appendListener(1, [&](int n) { ++calls; return n; });
	auto future = queue.enqueueWithResults(1, 2);
	EQ::QueuedEvent copy;
	REQUIRE(queue.peekEvent(&copy));
	queue.enqueue(0, 0);
	REQUIRE_THROWS_AS(queue.process(), std::runtime_error);
	REQUIRE(ready(future));
	REQUIRE(brokenPromise(future));
	queue.dispatch(copy);
	REQUIRE(calls == 0);
}

TEST_CASE("EventQueue results, wait notification and processing on another thread")
{
	Queue queue;
	queue.appendListener(1, [](int n) { return n * 2; });
	std::thread worker([&] { queue.wait(); queue.process(); });
	auto future = queue.enqueueWithResults(1, 21);
	const bool completed = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
	if(! completed) { queue.enqueue(1, 0); }
	worker.join();
	REQUIRE(completed);
	REQUIRE(future.get().results == std::vector<int> {42});
}

TEST_CASE("EventQueue results, processing predicate failure cancels the abandoned batch")
{
	Queue queue;
	auto first = queue.enqueueWithResults(1, 1);
	auto second = queue.enqueueWithResults(1, 2);
	Queue::QueuedEvent copy;
	REQUIRE(queue.peekEvent(&copy));
	auto predicate = [](int) -> bool { throw std::logic_error("predicate failed"); };
	SECTION("processIf") { REQUIRE_THROWS_AS(queue.processIf(predicate), std::logic_error); }
	SECTION("processUntil") { REQUIRE_THROWS_AS(queue.processUntil(predicate), std::logic_error); }
	REQUIRE(ready(first));
	REQUIRE(ready(second));
	REQUIRE(brokenPromise(first));
	REQUIRE(brokenPromise(second));
}
