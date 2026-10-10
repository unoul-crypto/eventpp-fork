#include "test.h"
#include "test_threading.h"
#include "eventpp/eventqueue.h"
#include "eventpp/mixins/mixinfilter.h"

#include <atomic>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace {
using Dispatcher = eventpp::EventDispatcher<int, int(int)>;
struct PointerPolicies { using AggregationResult = int; };
struct FilterPolicies { using Mixins = eventpp::MixinList<eventpp::MixinFilter>; };
struct CancelEvent { bool cancelled = false; };
struct CancelPolicies {
	static bool canContinueInvoking(const CancelEvent & event) { return !event.cancelled; }
};
struct ThrowOnCopy {
	ThrowOnCopy() = default;
	ThrowOnCopy(const ThrowOnCopy &) { throw std::logic_error("copy"); }
};
template <typename T> struct HasContinuation {
	template <typename U> static auto test(int) -> decltype(std::declval<U>().setResultContinuation(
		1, std::declval<typename U::ResultContinuation>()), std::true_type());
	template <typename U> static std::false_type test(...);
	static constexpr bool value = decltype(test<T>(0))::value;
};
}

TEST_CASE("Result continuation stores the stopping result and aggregates the invoked prefix")
{
	Dispatcher dispatcher;
	auto first = dispatcher.appendListener(1, [](int n) { return n; });
	auto second = dispatcher.appendListener(1, [](int n) { return n + 1; });
	dispatcher.appendListener(1, [](int n) { return n + 2; });
	std::vector<int> observed;
	dispatcher.setResultContinuation(1, [&](const Dispatcher::Handle & handle, const int & result, const int & n) {
		REQUIRE(n == 5);
		REQUIRE(handle.lock() == (result == 5 ? first : second).lock());
		observed.push_back(result);
		return result < 6;
	});
	dispatcher.setResultAggregator(1, [](const std::vector<int> & values) {
		return std::accumulate(values.begin(), values.end(), 0);
	});
	auto report = dispatcher.dispatchWithResults(1, 5);
	REQUIRE(report.results == std::vector<int> {5, 6});
	REQUIRE(report.handles.size() == 2);
	REQUIRE(report.stoppedByResult);
	REQUIRE(*report.aggregate == 11);
	REQUIRE(observed == report.results);
}

TEST_CASE("Result continuation applies to ordinary dispatch and clearing preserves other configuration")
{
	Dispatcher dispatcher;
	int calls = 0;
	int aggregates = 0;
	dispatcher.appendListener(1, [&](int n) { ++calls; return n; });
	dispatcher.appendListener(1, [&](int n) { ++calls; return n; });
	dispatcher.setResultAggregator(1, [&](const std::vector<int> &) { ++aggregates; return 0; });
	dispatcher.setResultContinuation(1, [](const Dispatcher::Handle &, const int &, const int &) { return false; });
	dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Continue);
	dispatcher.clearListenerOrdering(1);
	dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Propagate);
	dispatcher.dispatch(1, 3);
	REQUIRE(calls == 1);
	REQUIRE(aggregates == 0);
	dispatcher.setResultContinuation(1, {});
	dispatcher.dispatch(1, 3);
	REQUIRE(calls == 3);
	REQUIRE_FALSE(dispatcher.dispatchWithResults(2, 3).stoppedByResult);
	dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Continue);
	dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("handler"); });
	dispatcher.clearResultContinuation(1);
	REQUIRE(dispatcher.dispatchWithReport(1, 3).errors.size() == 1);
}

TEST_CASE("Result continuation observes ordered and replacement arguments")
{
	Dispatcher dispatcher;
	for(int i = 0; i < 3; ++i) { dispatcher.appendListener(1, [i](int n) { return n + i; }); }
	dispatcher.setResultContinuation(1, [](const Dispatcher::Handle &, const int & result, const int & n) {
		return result == n;
	});
	dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList & listeners, const int &) {
		return Dispatcher::ListenerOrder {listeners[2].handle, listeners[0].handle};
	});
	REQUIRE(dispatcher.dispatchWithResults(1, 1).results == std::vector<int> {3});
	dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int &) {
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[0].handle, 10);
		plan.add(listeners[1].handle, 20);
		plan.add(listeners[2].handle, 30);
		return plan;
	});
	auto report = dispatcher.dispatchWithReport(1, 1);
	REQUIRE(report.results == std::vector<int> {10, 21});
	REQUIRE(report.stoppedByResult);
	dispatcher.clearResultContinuation(1);
	REQUIRE(dispatcher.dispatchWithResults(1, 1).results == std::vector<int> {10, 21, 32});
}

TEST_CASE("Result continuation skips failed handlers and its own errors propagate")
{
	eventpp::EventQueue<int, int(int)> queue;
	int observations = 0;
	queue.appendListener(1, [](int) -> int { throw std::runtime_error("handler"); });
	queue.appendListener(1, [](int n) { return n; });
	queue.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Continue);
	queue.setResultContinuation(1, [&](const Dispatcher::Handle &, const int &, const int &) {
		++observations;
		return false;
	});
	auto report = queue.dispatchWithReport(1, 7);
	REQUIRE(report.errors.size() == 1);
	REQUIRE(report.results == std::vector<int> {7});
	REQUIRE(observations == 1);
	queue.setResultContinuation(1, [](const Dispatcher::Handle &, const int &, const int &) -> bool {
		throw std::logic_error("observer");
	});
	REQUIRE_THROWS_WITH(queue.dispatch(1, 7), "observer");
	REQUIRE_THROWS_WITH(queue.dispatchWithReport(1, 7), "observer");
	auto future = queue.enqueueWithResults(1, 7);
	REQUIRE(queue.process());
	REQUIRE_THROWS_WITH(future.get(), "observer");
}

TEST_CASE("Result continuation configuration is captured and supports reentrant dispatch")
{
	Dispatcher dispatcher;
	for(int i = 0; i < 3; ++i) { dispatcher.appendListener(1, [i](int) { return i; }); }
	std::vector<int> nested;
	dispatcher.setResultContinuation(1, [&](const Dispatcher::Handle &, const int &, const int & n) {
		dispatcher.clearResultContinuation(1);
		nested = dispatcher.dispatchWithResults(1, n).results;
		return false;
	});
	REQUIRE(dispatcher.dispatchWithResults(1, 4).results == std::vector<int> {0});
	REQUIRE(nested == std::vector<int> {0, 1, 2});
	REQUIRE(dispatcher.dispatchWithResults(1, 4).results == nested);
}

TEST_CASE("Result continuation copies independent callable state and survives move and swap")
{
	Dispatcher original;
	original.appendListener(1, [](int n) { return n; });
	original.setResultContinuation(1, [count = 0](const Dispatcher::Handle &, const int &, const int &) mutable {
		return ++count < 3;
	});
	REQUIRE_FALSE(original.dispatchWithReport(1, 1).stoppedByResult);
	original.appendListener(1, [](int n) { return n; });
	Dispatcher copied(original);
	REQUIRE(copied.dispatchWithResults(1, 1).results.size() == 2);
	REQUIRE(original.dispatchWithResults(1, 1).results.size() == 2);
	Dispatcher moved(std::move(copied));
	Dispatcher other;
	other.swap(moved);
	REQUIRE(other.dispatchWithResults(1, 1).results.size() == 1);
}

TEST_CASE("Result continuation supports move-only values, filters and argument cancellation")
{
	SECTION("Move-only results") {
		using Owned = eventpp::EventDispatcher<int, std::unique_ptr<int>(), PointerPolicies>;
		Owned dispatcher;
		dispatcher.appendListener(1, [] { return std::make_unique<int>(9); });
		dispatcher.appendListener(1, [] { return std::make_unique<int>(10); });
		dispatcher.setResultContinuation(1, [](const Owned::Handle &, const std::unique_ptr<int> & value) { return *value != 9; });
		auto report = dispatcher.dispatchWithResults(1);
		REQUIRE(report.results.size() == 1);
		REQUIRE(*report.results[0] == 9);
		dispatcher.dispatch(1);
	}
	SECTION("Filter") {
		eventpp::EventDispatcher<int, int(int), FilterPolicies> dispatcher;
		dispatcher.appendListener(1, [](int n) { return n; });
		dispatcher.appendFilter([](int) { return false; });
		int calls = 0;
		dispatcher.setResultContinuation(1, [&](const decltype(dispatcher)::Handle &, const int &, const int &) { ++calls; return false; });
		REQUIRE_FALSE(dispatcher.dispatchWithResults(1, 0).stoppedByResult);
		REQUIRE(calls == 0);
	}
	SECTION("Argument cancellation still stops dispatch") {
		using Cancel = eventpp::EventDispatcher<int, int(CancelEvent &), CancelPolicies>;
		Cancel dispatcher;
		dispatcher.appendListener(1, [](CancelEvent & event) { event.cancelled = true; return 1; });
		dispatcher.appendListener(1, [](CancelEvent &) { return 2; });
		dispatcher.setResultContinuation(1, [](const Cancel::Handle &, const int &, const CancelEvent &) { return true; });
		CancelEvent event;
		auto report = dispatcher.dispatchWithResults(1, event);
		REQUIRE(report.results == std::vector<int> {1});
		REQUIRE_FALSE(report.stoppedByResult);
	}
	SECTION("Copyable reference results are observed as owned values") {
		eventpp::EventDispatcher<int, int &()> dispatcher;
		int value = 7;
		dispatcher.appendListener(1, [&]() -> int & { return value; });
		dispatcher.setResultContinuation(1, [&](const decltype(dispatcher)::Handle &, const int & result) {
			++value;
			return result != 7;
		});
		auto report = dispatcher.dispatchWithResults(1);
		REQUIRE(report.results == std::vector<int> {7});
		REQUIRE(value == 8);
		REQUIRE(report.stoppedByResult);
	}
	STATIC_REQUIRE_FALSE(HasContinuation<eventpp::EventDispatcher<int, void()>>::value);
	STATIC_REQUIRE_FALSE(HasContinuation<eventpp::EventDispatcher<int, std::unique_ptr<int> &()>>::value);
}

TEST_CASE("Result continuation concurrent configuration uses one snapshot per dispatch", "[thread]")
{
	Dispatcher dispatcher;
	for(int i = 0; i < 3; ++i) { dispatcher.appendListener(1, [i](int n) { return n + i; }); }
	test_threading::Barrier start(2);
	std::thread writer([&] {
		start.arriveAndWait();
		for(int i = 0; i < 1024; ++i) {
			dispatcher.setResultContinuation(1, [i](const Dispatcher::Handle &, const int &, const int &) { return i % 2 != 0; });
		}
	});
	start.arriveAndWait();
	bool valid = true;
	for(int i = 0; i < 1024; ++i) {
		auto report = dispatcher.dispatchWithResults(1, i);
		valid = valid && report.results.size() == (report.stoppedByResult ? 1 : 3);
	}
	writer.join();
	REQUIRE(valid);
}

TEST_CASE("Ordinary Continue dispatch does not copy ignored reference returns")
{
	eventpp::EventDispatcher<int, ThrowOnCopy &()> dispatcher;
	ThrowOnCopy value;
	dispatcher.appendListener(1, [&]() -> ThrowOnCopy & { return value; });
	dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Continue);
	REQUIRE_NOTHROW(dispatcher.dispatch(1));
	dispatcher.setResultContinuation(1, [](const decltype(dispatcher)::Handle &, const ThrowOnCopy &) { return true; });
	REQUIRE_THROWS_WITH(dispatcher.dispatch(1), "copy");
}
