#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"
#include "eventpp/mixins/mixinfilter.h"

#include <numeric>
#include <stdexcept>
#include <memory>
#include <atomic>
#include <thread>

namespace {
using Dispatcher = eventpp::EventDispatcher<int, int(int)>;

template <typename Handle>
bool sameHandle(const Handle & a, const Handle & b)
{
	return ! a.owner_before(b) && ! b.owner_before(a);
}

struct TextPolicies {
	using AggregationResult = std::string;
};

struct OwnedSummary {
	explicit OwnedSummary(int value) : value(value) {}
	OwnedSummary(OwnedSummary &&) = default;
	OwnedSummary(const OwnedSummary &) = delete;
	int value;
};
struct SummaryPolicies {
	using AggregationResult = OwnedSummary;
};
struct PointerPolicies {
	using AggregationResult = int;
};

struct CancelEvent {
	int type = 1;
	bool canceled = false;
};
struct CancelPolicies {
	static int getEvent(const CancelEvent & event) { return event.type; }
	static bool canContinueInvoking(const CancelEvent & event) { return ! event.canceled; }
};
struct FilterPolicies {
	using Mixins = eventpp::MixinList<eventpp::MixinFilter>;
};

template <typename T>
struct HasResultDispatch {
	template <typename U> static auto test(int) -> decltype(std::declval<U>().dispatchWithResults(1), std::true_type());
	template <typename U> static std::false_type test(...);
	static constexpr bool value = decltype(test<T>(0))::value;
};

struct AbstractResult {
	virtual ~AbstractResult() = default;
	virtual int value() const = 0;
};
struct ConcreteResult : AbstractResult {
	int value() const override { return 7; }
};
}

TEST_CASE("EventDispatcher results, values, matching handles and aggregation")
{
	Dispatcher dispatcher;
	auto a = dispatcher.appendListener(1, [](int n) { return n + 1; });
	auto b = dispatcher.appendListener(1, [](int n) { return n + 2; });
	int aggregateCalls = 0;
	dispatcher.setResultAggregator(1, [&](const Dispatcher::ListenerResults & values) {
		++aggregateCalls;
		return std::accumulate(values.begin(), values.end(), 0);
	});
	auto result = dispatcher.dispatchWithResults(1, 10);
	REQUIRE(result.results == std::vector<int> {11, 12});
	REQUIRE(result.handles.size() == 2);
	REQUIRE(sameHandle(result.handles[0], a));
	REQUIRE(sameHandle(result.handles[1], b));
	REQUIRE(*result.aggregate == 23);
	REQUIRE(aggregateCalls == 1);
	dispatcher.dispatch(1, 10);
	REQUIRE(aggregateCalls == 1);
}

TEST_CASE("EventDispatcher results, optional aggregator, event isolation and reset")
{
	Dispatcher dispatcher;
	dispatcher.appendListener(1, [](int n) { return n; });
	dispatcher.appendListener(2, [](int n) { return n + 1; });
	auto plain = dispatcher.dispatchWithResults(1, 7);
	REQUIRE(plain.results == std::vector<int> {7});
	REQUIRE_FALSE(plain.aggregate);
	dispatcher.setResultAggregator(1, [](const std::vector<int> &) { return 42; });
	REQUIRE(*dispatcher.dispatchWithResults(1, 7).aggregate == 42);
	REQUIRE_FALSE(dispatcher.dispatchWithResults(2, 7).aggregate);
	dispatcher.clearResultAggregator(1);
	REQUIRE_FALSE(dispatcher.dispatchWithResults(1, 7).aggregate);
	dispatcher.setResultAggregator(1, [](const std::vector<int> &) { return 99; });
	dispatcher.setResultAggregator(1, {});
	REQUIRE_FALSE(dispatcher.dispatchWithResults(1, 7).aggregate);
	dispatcher.clearResultAggregator(999);
}

TEST_CASE("EventDispatcher results, different and non-default-constructible aggregate types")
{
	using TextED = eventpp::EventDispatcher<int, int(), TextPolicies>;
	TextED text;
	text.appendListener(1, [] { return 3; });
	text.setResultAggregator(1, [](const std::vector<int> & values) { return std::to_string(values[0]); });
	auto textResult = text.dispatchWithResults(1);
	REQUIRE(*textResult.aggregate == "3");

	using SummaryED = eventpp::EventDispatcher<int, int(), SummaryPolicies>;
	SummaryED owned;
	owned.appendListener(1, [] { return 5; });
	owned.setResultAggregator(1, [](const std::vector<int> & values) { return OwnedSummary(values[0]); });
	auto summary = owned.dispatchWithResults(1);
	REQUIRE(summary.aggregate->value == 5);
}

TEST_CASE("EventDispatcher results, ordering selection and invalid handles")
{
	Dispatcher dispatcher, other;
	auto a = dispatcher.appendListener(1, [](int) { return 1; });
	auto b = dispatcher.appendListener(1, [](int) { return 2; });
	auto removed = dispatcher.appendListener(1, [](int) { return 3; });
	auto foreign = other.appendListener(1, [](int) { return 4; });
	dispatcher.setListenerOrdering(1, [&](const Dispatcher::ListenerList &, const int &) {
		dispatcher.removeListener(1, removed);
		auto added = dispatcher.appendListener(1, [](int) { return 5; });
		return Dispatcher::ListenerOrder {{}, foreign, removed, b, b, added, a};
	});
	auto result = dispatcher.dispatchWithResults(1, 0);
	REQUIRE(result.results == std::vector<int> {2, 1});
	REQUIRE(result.handles.size() == 2);
	REQUIRE(sameHandle(result.handles[0], b));
	REQUIRE(sameHandle(result.handles[1], a));
}

TEST_CASE("EventDispatcher results, listener plan replacement arguments")
{
	Dispatcher dispatcher;
	auto a = dispatcher.appendListener(1, [](int n) { return n + 1; });
	auto b = dispatcher.appendListener(1, [](int n) { return n + 2; });
	dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int & n) {
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[1].handle, n * 2);
		plan.add(listeners[0].handle);
		return plan;
	});
	dispatcher.setResultAggregator(1, [](const std::vector<int> & values) { return values[0] + values[1]; });
	auto result = dispatcher.dispatchWithResults(1, 10);
	REQUIRE(result.results == std::vector<int> {22, 11});
	REQUIRE(sameHandle(result.handles[0], b));
	REQUIRE(sameHandle(result.handles[1], a));
	REQUIRE(*result.aggregate == 33);
}

TEST_CASE("EventDispatcher results, reference returns survive plan destruction")
{
	using ED = eventpp::EventDispatcher<int, std::string &(std::string &)>;
	ED dispatcher;
	dispatcher.appendListener(1, [](std::string & s) -> std::string & { s += "!"; return s; });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const std::string &) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, std::string("owned"));
		return plan;
	});
	std::string original = "original";
	auto result = dispatcher.dispatchWithResults(1, original);
	REQUIRE(result.results == std::vector<std::string> {"owned!"});
	REQUIRE(original == "original");
}

TEST_CASE("EventDispatcher results, subscription mutations preserve result traversal")
{
	Dispatcher dispatcher;
	Dispatcher::Handle second;
	bool changed = false;
	auto first = dispatcher.appendListener(1, [&](int n) {
		if(! changed) {
			changed = true;
			dispatcher.removeListener(1, second);
			// New subscriptions do not extend the dispatch already in progress.
			dispatcher.appendListener(1, [](int value) { return value + 30; });
		}
		return n + 10;
	});
	second = dispatcher.appendListener(1, [](int n) { return n + 20; });
	auto initial = dispatcher.dispatchWithResults(1, 1);
	REQUIRE(initial.results == std::vector<int> {11});
	REQUIRE(initial.handles.size() == 1);
	REQUIRE(sameHandle(initial.handles[0], first));
	auto later = dispatcher.dispatchWithResults(1, 2);
	REQUIRE(later.results == std::vector<int> {12, 32});
	REQUIRE(later.handles.size() == 2);
}

TEST_CASE("EventDispatcher results, move-only handler returns")
{
	using ED = eventpp::EventDispatcher<int, std::unique_ptr<int>(), PointerPolicies>;
	ED dispatcher;
	dispatcher.appendListener(1, [] { return std::unique_ptr<int>(new int(3)); });
	dispatcher.appendListener(1, [] { return std::unique_ptr<int>(new int(5)); });
	dispatcher.setResultAggregator(1, [](const ED::ListenerResults & values) { return *values[0] + *values[1]; });
	auto result = dispatcher.dispatchWithResults(1);
	REQUIRE(*result.results[0] == 3);
	REQUIRE(*result.results[1] == 5);
	REQUIRE(*result.aggregate == 8);
}

TEST_CASE("EventDispatcher results, cancellation with original and replacement arguments")
{
	using ED = eventpp::EventDispatcher<int, int(CancelEvent &), CancelPolicies>;
	ED dispatcher;
	dispatcher.appendListener(1, [](CancelEvent & event) { event.canceled = true; return 1; });
	dispatcher.appendListener(1, [](CancelEvent &) { return 2; });
	dispatcher.setResultAggregator(1, [](const std::vector<int> & values) { return int(values.size()); });
	CancelEvent event;
	auto result = dispatcher.dispatchWithResults(event);
	REQUIRE(event.canceled);
	REQUIRE(result.results == std::vector<int> {1});
	REQUIRE(*result.aggregate == 1);
	event.canceled = false;
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const CancelEvent &) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, CancelEvent {});
		plan.add(listeners[1].handle);
		return plan;
	});
	auto planned = dispatcher.dispatchWithResults(event);
	REQUIRE_FALSE(event.canceled);
	REQUIRE(planned.results == std::vector<int> {1});
	REQUIRE(*planned.aggregate == 1);
	dispatcher.appendListener(3, [](CancelEvent &) { return 10; });
	REQUIRE(dispatcher.directDispatchWithResults(3, event).results == std::vector<int> {10});
}

TEST_CASE("EventDispatcher results, empty events, plans and filters")
{
	using ED = eventpp::EventDispatcher<int, int(int &), FilterPolicies>;
	ED dispatcher;
	int aggregateCalls = 0;
	dispatcher.setResultAggregator(1, [&](const std::vector<int> & values) { ++aggregateCalls; return int(values.size()); });
	int n = 3;
	auto empty = dispatcher.dispatchWithResults(1, n);
	REQUIRE(empty.results.empty());
	REQUIRE(empty.handles.empty());
	REQUIRE(*empty.aggregate == 0);
	dispatcher.appendListener(1, [](int & value) { return value; });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList &, const int &) { return ED::ListenerPlan {}; });
	REQUIRE(dispatcher.dispatchWithResults(1, n).results.empty());
	dispatcher.clearListenerPlanner(1);
	dispatcher.appendFilter([](int & value) { value += 1; return value > 0; });
	REQUIRE(dispatcher.dispatchWithResults(1, n).results == std::vector<int> {4});
	n = -10;
	auto filtered = dispatcher.dispatchWithResults(1, n);
	REQUIRE(filtered.results.empty());
	REQUIRE(*filtered.aggregate == 0);
	REQUIRE(aggregateCalls == 4);
}

TEST_CASE("EventDispatcher results, nested dispatch and aggregator configuration snapshot")
{
	Dispatcher dispatcher;
	dispatcher.appendListener(2, [](int n) { return n * 3; });
	dispatcher.appendListener(1, [&](int n) {
		dispatcher.setResultAggregator(1, [](const std::vector<int> &) { return 99; });
		return dispatcher.dispatchWithResults(2, n).results[0];
	});
	dispatcher.setResultAggregator(1, [&](const std::vector<int> & values) {
		dispatcher.clearResultAggregator(2);
		return values[0] + 1;
	});
	REQUIRE(*dispatcher.dispatchWithResults(1, 2).aggregate == 7);
	REQUIRE(*dispatcher.dispatchWithResults(1, 2).aggregate == 99);
}

TEST_CASE("EventDispatcher results, aggregator copy, assignment, move and swap")
{
	Dispatcher dispatcher;
	dispatcher.appendListener(1, [](int n) { return n; });
	dispatcher.setResultAggregator(1, [count = 0](const std::vector<int> &) mutable { return ++count; });
	REQUIRE(*dispatcher.dispatchWithResults(1, 0).aggregate == 1);
	Dispatcher copied(dispatcher), assigned;
	assigned = dispatcher;
	REQUIRE(*copied.dispatchWithResults(1, 0).aggregate == 2);
	REQUIRE(*assigned.dispatchWithResults(1, 0).aggregate == 2);
	REQUIRE(*dispatcher.dispatchWithResults(1, 0).aggregate == 2);
	Dispatcher moved(std::move(copied)), moveAssigned;
	moveAssigned = std::move(assigned);
	REQUIRE(*moved.dispatchWithResults(1, 0).aggregate == 3);
	REQUIRE(*moveAssigned.dispatchWithResults(1, 0).aggregate == 3);
	Dispatcher swapped;
	swap(swapped, moved);
	REQUIRE(*swapped.dispatchWithResults(1, 0).aggregate == 4);
}

TEST_CASE("EventDispatcher results, exceptions propagate and release collected values")
{
	using ED = eventpp::EventDispatcher<int, std::shared_ptr<int>(), PointerPolicies>;
	ED dispatcher;
	std::weak_ptr<int> lifetime;
	dispatcher.appendListener(1, [&] { auto value = std::make_shared<int>(7); lifetime = value; return value; });
	auto throwing = dispatcher.appendListener(1, []() -> std::shared_ptr<int> { throw std::runtime_error("listener failed"); });
	int aggregates = 0;
	dispatcher.setResultAggregator(1, [&](const ED::ListenerResults &) { ++aggregates; return 0; });
	REQUIRE_THROWS_AS(dispatcher.dispatchWithResults(1), std::runtime_error);
	REQUIRE(aggregates == 0);
	REQUIRE(lifetime.expired());
	dispatcher.removeListener(1, throwing);
	dispatcher.setResultAggregator(1, [](const ED::ListenerResults &) -> int { throw std::runtime_error("aggregator failed"); });
	REQUIRE_THROWS_AS(dispatcher.dispatchWithResults(1), std::runtime_error);
	REQUIRE(lifetime.expired());
	dispatcher.clearResultAggregator(1);
	auto result = dispatcher.dispatchWithResults(1);
	REQUIRE(*result.results[0] == 7);
}

TEST_CASE("EventDispatcher results, void and non-ownable returns preserve ordinary dispatch")
{
	using VoidED = eventpp::EventDispatcher<int, void()>;
	static_assert(! HasResultDispatch<VoidED>::value, "Void handlers cannot collect values.");
	using RefED = eventpp::EventDispatcher<int, AbstractResult &()>;
	static_assert(! HasResultDispatch<RefED>::value, "Abstract results cannot be stored as values.");
	RefED dispatcher;
	ConcreteResult value;
	int calls = 0;
	dispatcher.appendListener(1, [&]() -> AbstractResult & { ++calls; return value; });
	dispatcher.setListenerPlanner(1, [](const RefED::ListenerList & listeners) {
		RefED::ListenerPlan plan;
		plan.add(listeners[0].handle);
		return plan;
	});
	dispatcher.dispatch(1);
	REQUIRE(calls == 1);
}

TEST_CASE("EventQueue results, synchronous collection and ordinary queue processing")
{
	using EQ = eventpp::EventQueue<int, int(int)>;
	EQ queue;
	queue.appendListener(1, [](int n) { return n * 2; });
	int aggregates = 0;
	queue.setResultAggregator(1, [&](const std::vector<int> & values) { ++aggregates; return values[0]; });
	auto result = queue.dispatchWithResults(1, 3);
	REQUIRE(result.results == std::vector<int> {6});
	REQUIRE(*result.aggregate == 6);
	queue.enqueue(1, 5);
	REQUIRE(queue.process());
	REQUIRE(aggregates == 1);
}

TEST_CASE("EventDispatcher results, concurrent collections have independent vectors", "[thread]")
{
	Dispatcher dispatcher;
	dispatcher.appendListener(1, [](int n) { return n * 2; });
	dispatcher.setResultAggregator(1, [](const std::vector<int> & values) { return values[0] + 1; });
	std::atomic<bool> failed {false};
	auto collect = [&] {
		for(int i = 0; i < 200; ++i) {
			auto result = dispatcher.dispatchWithResults(1, i);
			if(result.results != std::vector<int> {i * 2} || *result.aggregate != i * 2 + 1) { failed = true; }
		}
	};
	std::thread first(collect), second(collect);
	first.join();
	second.join();
	REQUIRE_FALSE(failed);
}
