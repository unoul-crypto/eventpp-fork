#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"
#include "eventpp/mixins/mixinfilter.h"

#include <memory>
#include <numeric>
#include <stdexcept>
#include <atomic>
#include <thread>

namespace {
using Dispatcher = eventpp::EventDispatcher<int, int(int)>;
using Policy = eventpp::ListenerExceptionPolicy;
template <typename Handle>
bool sameOwner(const Handle & a, const Handle & b) {
	return !a.owner_before(b) && !b.owner_before(a);
}
struct CancelEvent { bool cancelled = false; };
struct CancelPolicies {
	static bool canContinueInvoking(const CancelEvent & event) { return !event.cancelled; }
};
struct ContinuationThrows {
	static bool canContinueInvoking(int) { throw std::logic_error("continuation failed"); }
};
struct FilterPolicies { using Mixins = eventpp::MixinList<eventpp::MixinFilter>; };
struct Unstorable {
	Unstorable() = default;
	Unstorable(const Unstorable &) { throw std::logic_error("storage failed"); }
};
struct Abstract { virtual int value() const = 0; virtual ~Abstract() = default; };
struct Concrete : Abstract { int value() const override { return 7; } };
}

TEST_CASE("Listener exceptions, default propagation and continued results with errors")
{
	Dispatcher dispatcher;
	int after = 0;
	auto first = dispatcher.appendListener(1, [](int n) { return n + 1; });
	auto broken = dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("handler failed"); });
	auto last = dispatcher.appendListener(1, [&](int n) { ++after; return n * 3; });
	dispatcher.setResultAggregator(1, [](const Dispatcher::ListenerResults & values) {
		return std::accumulate(values.begin(), values.end(), 0);
	});
	REQUIRE_THROWS_AS(dispatcher.dispatchWithResults(1, 5), std::runtime_error);
	REQUIRE(after == 0);
	dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
	auto result = dispatcher.dispatchWithResults(1, 5);
	REQUIRE(result.results == std::vector<int> {6, 15});
	REQUIRE(result.handles.size() == 2);
	REQUIRE(sameOwner(result.handles[0], first));
	REQUIRE(sameOwner(result.handles[1], last));
	REQUIRE(*result.aggregate == 21);
	REQUIRE(result.errors.size() == 1);
	REQUIRE(sameOwner(result.errors[0].handle, broken));
	REQUIRE_THROWS_WITH(std::rethrow_exception(result.errors[0].exception), "handler failed");
	REQUIRE(after == 1);
	dispatcher.dispatch(1, 5); // Ordinary dispatch discards the recorded error and values.
	REQUIRE(after == 2);
}

TEST_CASE("Listener exceptions, policy and selection are independent and event-local")
{
	Dispatcher dispatcher;
	dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
	dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("first"); });
	dispatcher.appendListener(1, [](int n) { return n; });
	dispatcher.appendListener(2, [](int) -> int { throw std::runtime_error("other event"); });
	auto reverse = [](const Dispatcher::ListenerList & listeners, const int &) {
		return Dispatcher::ListenerOrder {listeners[1].handle, listeners[0].handle};
	};
	dispatcher.setListenerOrdering(1, reverse);
	REQUIRE(dispatcher.dispatchWithReport(1, 3).results == std::vector<int> {3});
	dispatcher.clearListenerOrdering(1);
	REQUIRE(dispatcher.dispatchWithReport(1, 3).errors.size() == 1);
	dispatcher.setListenerOrdering(1, reverse);
	dispatcher.setListenerExceptionPolicy(1, Policy::Propagate);
	int selections = 0;
	dispatcher.setListenerOrdering(1, [&](const Dispatcher::ListenerList & listeners, const int & n) {
		++selections;
		return reverse(listeners, n);
	});
	REQUIRE_THROWS_AS(dispatcher.dispatchWithReport(1, 3), std::runtime_error);
	REQUIRE(selections == 1);
	REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(2, 0), "other event");
}

TEST_CASE("Listener exceptions, plans and invalid selections preserve error handle identity")
{
	Dispatcher dispatcher, other;
	auto a = dispatcher.appendListener(1, [](int n) { return n + 1; });
	auto b = dispatcher.appendListener(1, [](int n) -> int {
		if(n == 9) { throw std::runtime_error("replacement failed"); }
		return n;
	});
	auto foreign = other.appendListener(1, [](int) -> int { throw std::runtime_error("must not run"); });
	dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
	dispatcher.setListenerPlanner(1, [=](const Dispatcher::ListenerList &, const int &) {
		Dispatcher::ListenerPlan plan;
		plan.add(foreign, 9);
		plan.add(b, 9);
		plan.add(b, 0);
		plan.add(a, 4);
		return plan;
	});
	auto result = dispatcher.dispatchWithReport(1, 0);
	REQUIRE(result.results == std::vector<int> {5});
	REQUIRE(result.errors.size() == 1);
	REQUIRE(sameOwner(result.errors[0].handle, b));
	REQUIRE(sameOwner(result.handles[0], a));
	REQUIRE_THROWS_WITH(std::rethrow_exception(result.errors[0].exception), "replacement failed");
	dispatcher.clearListenerPlanner(1);
	REQUIRE(dispatcher.dispatchWithReport(1, 0).errors.empty());
}

TEST_CASE("Listener exceptions, policy snapshots remain stable during nested dispatch")
{
	Dispatcher dispatcher;
	dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
	dispatcher.appendListener(1, [&](int depth) {
		if(depth == 0) {
			dispatcher.setListenerExceptionPolicy(1, Policy::Propagate);
			REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1, 1), "nested failure");
		}
		return depth;
	});
	dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("nested failure"); });
	dispatcher.appendListener(1, [](int) { return 20; });
	auto outer = dispatcher.dispatchWithReport(1, 0);
	REQUIRE(outer.results == std::vector<int> {0, 20});
	REQUIRE(outer.errors.size() == 1);
	REQUIRE_THROWS_AS(dispatcher.dispatchWithReport(1, 2), std::runtime_error);
}

TEST_CASE("Listener exceptions, void reports and cancellation after a failed handler")
{
	using ED = eventpp::EventDispatcher<int, void(CancelEvent &), CancelPolicies>;
	ED dispatcher;
	int after = 0;
	auto broken = dispatcher.appendListener(1, [](CancelEvent & event) {
		event.cancelled = true;
		throw std::runtime_error("cancelled failure");
	});
	dispatcher.appendListener(1, [&](CancelEvent &) { ++after; });
	dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
	CancelEvent event;
	auto report = dispatcher.dispatchWithReport(1, event);
	REQUIRE(report.results.empty());
	REQUIRE(report.handles.empty());
	REQUIRE(report.errors.size() == 1);
	REQUIRE(sameOwner(report.errors[0].handle, broken));
	REQUIRE(after == 0);
	event.cancelled = false;
	dispatcher.dispatch(1, event);
	REQUIRE(after == 0);
}

TEST_CASE("Listener exceptions, policy follows dispatcher copy, move, assignment and swap")
{
	Dispatcher source;
	source.appendListener(1, [](int) -> int { throw std::runtime_error("failure"); });
	source.appendListener(1, [](int n) { return n; });
	source.setListenerExceptionPolicy(1, Policy::Continue);
	Dispatcher copied(source), assigned;
	assigned = source;
	source.setListenerExceptionPolicy(1, Policy::Propagate);
	Dispatcher moved(std::move(copied)), moveAssigned;
	moveAssigned = std::move(assigned);
	Dispatcher swapped;
	swap(swapped, moved);
	REQUIRE(swapped.dispatchWithReport(1, 2).errors.size() == 1);
	REQUIRE(moveAssigned.dispatchWithReport(1, 3).results == std::vector<int> {3});
	REQUIRE_THROWS_AS(source.dispatchWithReport(1, 0), std::runtime_error);
}

TEST_CASE("Listener exceptions, selection, aggregation, filter and continuation failures propagate")
{
	SECTION("Ordering and planner") {
		Dispatcher dispatcher;
		dispatcher.appendListener(1, [](int n) { return n; });
		dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
		dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList &, const int &) -> Dispatcher::ListenerOrder {
			throw std::logic_error("ordering failed");
		});
		REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1, 0), "ordering failed");
		dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList &, const int &) -> Dispatcher::ListenerPlan {
			throw std::logic_error("planner failed");
		});
		REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1, 0), "planner failed");
	}
	SECTION("Aggregator") {
		Dispatcher dispatcher;
		dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("callback failed"); });
		dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
		dispatcher.setResultAggregator(1, [](const Dispatcher::ListenerResults & values) -> int {
			REQUIRE(values.empty());
			throw std::logic_error("aggregator failed");
		});
		REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1, 0), "aggregator failed");
	}
	SECTION("Continuation") {
		eventpp::EventDispatcher<int, int(int), ContinuationThrows> dispatcher;
		dispatcher.appendListener(1, [](int n) { return n; });
		dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
		REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1, 0), "continuation failed");
	}
	SECTION("Filter") {
		eventpp::EventDispatcher<int, int(int), FilterPolicies> dispatcher;
		dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
		dispatcher.appendFilter([](int) -> bool { throw std::logic_error("filter failed"); });
		REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1, 0), "filter failed");
	}
}

TEST_CASE("Listener exceptions, result storage exceptions are not treated as callback errors")
{
	eventpp::EventDispatcher<int, const Unstorable &()> dispatcher;
	Unstorable value;
	int after = 0;
	dispatcher.appendListener(1, [&]() -> const Unstorable & { return value; });
	dispatcher.appendListener(1, [&]() -> const Unstorable & { ++after; return value; });
	dispatcher.setListenerExceptionPolicy(1, Policy::Continue);
	REQUIRE_THROWS_WITH(dispatcher.dispatchWithReport(1), "storage failed");
	REQUIRE(after == 0);
	dispatcher.dispatch(1);
	REQUIRE(after == 1);
}

TEST_CASE("Listener exceptions, move-only results and abstract-reference reports")
{
	eventpp::EventDispatcher<int, std::unique_ptr<int>()> owned;
	owned.setListenerExceptionPolicy(1, Policy::Continue);
	owned.appendListener(1, [] { return std::unique_ptr<int>(new int(3)); });
	owned.appendListener(1, []() -> std::unique_ptr<int> { throw std::runtime_error("failure"); });
	owned.appendListener(1, [] { return std::unique_ptr<int>(new int(7)); });
	auto values = owned.dispatchWithReport(1);
	REQUIRE(values.results.size() == 2);
	REQUIRE(*values.results[0] == 3);
	REQUIRE(*values.results[1] == 7);
	REQUIRE(values.errors.size() == 1);

	eventpp::EventDispatcher<int, Abstract &()> refs;
	Concrete value;
	refs.setListenerExceptionPolicy(1, Policy::Continue);
	refs.appendListener(1, [&]() -> Abstract & { return value; });
	refs.appendListener(1, []() -> Abstract & { throw std::runtime_error("reference failure"); });
	auto report = refs.dispatchWithReport(1);
	REQUIRE(report.results.empty());
	REQUIRE(report.handles.empty());
	REQUIRE(report.errors.size() == 1);
}

TEST_CASE("Listener exceptions, queued results choose the processing-time policy")
{
	using Queue = eventpp::EventQueue<int, int(int)>;
	Queue queue;
	queue.appendListener(1, [](int) -> int { throw std::runtime_error("queue failure"); });
	queue.appendListener(1, [](int n) { return n * 2; });
	auto first = queue.enqueueWithResults(1, 3);
	queue.setListenerExceptionPolicy(1, Policy::Continue);
	REQUIRE(queue.process());
	auto report = first.get();
	REQUIRE(report.results == std::vector<int> {6});
	REQUIRE(report.errors.size() == 1);
	queue.setListenerExceptionPolicy(1, Policy::Propagate);
	auto second = queue.enqueueWithReport(1, 4);
	REQUIRE(queue.process());
	REQUIRE_THROWS_WITH(second.get(), "queue failure");
}

TEST_CASE("Listener exceptions, void queue reports share completion and cancellation semantics")
{
	eventpp::EventQueue<int, void()> queue;
	int after = 0;
	auto broken = queue.appendListener(1, [] { throw std::runtime_error("void failure"); });
	queue.appendListener(1, [&] { ++after; });
	queue.setListenerExceptionPolicy(1, Policy::Continue);
	auto future = queue.enqueueWithReport(1);
	decltype(queue)::QueuedEvent peek;
	REQUIRE(queue.peekEvent(&peek));
	REQUIRE(queue.process());
	queue.dispatch(peek); // A saved copy must not invoke completed work again.
	auto report = future.get();
	REQUIRE(report.errors.size() == 1);
	REQUIRE(sameOwner(report.errors[0].handle, broken));
	REQUIRE(report.results.empty());
	REQUIRE(after == 1);
	REQUIRE(queue.removeListener(1, broken));
	REQUIRE(report.errors[0].handle.expired());
	REQUIRE_THROWS_WITH(std::rethrow_exception(report.errors[0].exception), "void failure");
	auto cancelled = queue.enqueueWithReport(1);
	REQUIRE(queue.peekEvent(&peek));
	queue.clearEvents();
	queue.dispatch(peek);
	REQUIRE_THROWS_AS(cancelled.get(), std::future_error);
	REQUIRE(after == 1);
	queue.setListenerExceptionPolicy(2, Policy::Propagate);
	queue.appendListener(2, [] { throw std::runtime_error("propagated void failure"); });
	auto propagated = queue.enqueueWithReport(2);
	REQUIRE(queue.process());
	REQUIRE_THROWS_WITH(propagated.get(), "propagated void failure");
}

TEST_CASE("Listener exceptions, custom event extraction works for report APIs")
{
	struct Event { int type; int value; };
	struct EventPolicies { static int getEvent(const Event & event) { return event.type; } };
	eventpp::EventQueue<int, int(Event), EventPolicies> queue;
	queue.appendListener(7, [](Event event) { return event.value; });
	REQUIRE(queue.dispatchWithReport(Event {7, 9}).results == std::vector<int> {9});
	REQUIRE(queue.directDispatchWithReport(7, Event {1, 8}).results == std::vector<int> {8});
	auto future = queue.enqueueWithReport(Event {7, 10});
	REQUIRE(queue.process());
	REQUIRE(future.get().results == std::vector<int> {10});
}

TEST_CASE("Listener exceptions, concurrent policy changes keep each dispatch consistent", "[thread]")
{
	Dispatcher dispatcher;
	dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("failure"); });
	dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("failure"); });
	dispatcher.appendListener(1, [](int n) { return n; });
	std::atomic<bool> failed {false};
	std::atomic<int> completed {0};
	std::thread writer([&] {
		for(int n = 0; n < 1000; ++n) {
			dispatcher.setListenerExceptionPolicy(1, n % 2 ? Policy::Continue : Policy::Propagate);
		}
	});
	auto consume = [&] {
		for(int n = 0; n < 500; ++n) {
			try {
				auto report = dispatcher.dispatchWithReport(1, n);
				if(report.results != std::vector<int>{n} || report.errors.size() != 2) { failed = true; }
			}
			catch(const std::runtime_error &) {}
			catch(...) { failed = true; }
			++completed;
		}
	};
	std::thread first(consume), second(consume);
	writer.join();
	first.join();
	second.join();
	REQUIRE_FALSE(failed);
	REQUIRE(completed == 1000);
}

TEST_CASE("Listener exceptions, competing queued report dispatch completes only once", "[thread]")
{
	eventpp::EventQueue<int, void()> queue;
	std::atomic<int> completed {0};
	queue.appendListener(1, [] { throw std::runtime_error("failure"); });
	queue.appendListener(1, [&] { ++completed; });
	queue.setListenerExceptionPolicy(1, Policy::Continue);
	auto future = queue.enqueueWithReport(1);
	decltype(queue)::QueuedEvent item;
	REQUIRE(queue.takeEvent(&item));
	std::vector<std::thread> workers;
	for(int n = 0; n < 8; ++n) { workers.emplace_back([&] { queue.dispatch(item); }); }
	for(auto & worker : workers) { worker.join(); }
	auto report = future.get();
	REQUIRE(completed == 1);
	REQUIRE(report.errors.size() == 1);
	REQUIRE_THROWS_WITH(std::rethrow_exception(report.errors[0].exception), "failure");
}
