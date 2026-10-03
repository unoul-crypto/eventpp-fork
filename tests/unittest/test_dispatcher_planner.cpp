#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"
#include "eventpp/mixins/mixinfilter.h"

#include <memory>
#include <stdexcept>
#include <atomic>
#include <thread>

namespace {
using Dispatcher = eventpp::EventDispatcher<int, void(int, const std::string &)>;

struct CancelEvent {
	bool canceled = false;
};

struct CancelPolicies {
	static bool canContinueInvoking(const CancelEvent & event) { return ! event.canceled; }
};

struct FilterPolicies {
	using Mixins = eventpp::MixinList<eventpp::MixinFilter>;
};

struct AbstractValue {
	virtual ~AbstractValue() = default;
	virtual int value() const = 0;
};
struct ConcreteValue : AbstractValue {
	int value() const override { return 7; }
};
}

TEST_CASE("EventDispatcher planner, individual arguments, selection and order")
{
	Dispatcher dispatcher;
	std::vector<std::string> calls;
	dispatcher.appendListener(1, [&](int n, const std::string & s) { calls.push_back("A" + s + std::to_string(n)); }, {{"id", "A"}});
	dispatcher.appendListener(1, [&](int n, const std::string & s) { calls.push_back("B" + s + std::to_string(n)); }, {{"id", "B"}});
	dispatcher.appendListener(1, [&](int, const std::string &) { calls.push_back("skip"); });
	dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int & n, const std::string & s) {
		REQUIRE(listeners[0].metadata.at("id") == "A");
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[1].handle, n * 2, s + "!");
		plan.add(listeners[0].handle);
		REQUIRE(plan.size() == 2);
		return plan;
	});
	std::string text = "original";
	dispatcher.dispatch(1, 3, text);
	REQUIRE(calls == std::vector<std::string> {"Boriginal!6", "Aoriginal3"});
	REQUIRE(text == "original");
}

TEST_CASE("EventDispatcher planner, replacement references are isolated")
{
	using ED = eventpp::EventDispatcher<int, void(int &)>;
	ED dispatcher;
	std::vector<int> calls;
	int original = 5;
	dispatcher.appendListener(1, [&](int & n) { REQUIRE(&n == &original); n += 1; calls.push_back(n); });
	dispatcher.appendListener(1, [&](int & n) { REQUIRE(&n != &original); calls.push_back(n); n = 999; });
	dispatcher.appendListener(1, [&](int & n) { REQUIRE(&n != &original); calls.push_back(n); });
	dispatcher.appendListener(1, [&](int & n) { REQUIRE(&n == &original); calls.push_back(n); });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const int & n) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle);
		plan.add(listeners[1].handle, n * 2);
		plan.add(listeners[2].handle, n * 3);
		plan.add(listeners[3].handle);
		return plan;
	});
	dispatcher.dispatch(1, original);
	REQUIRE(original == 6);
	REQUIRE(calls == std::vector<int> {6, 10, 15, 6});
}

TEST_CASE("EventDispatcher planner, replacement conversions and zero argument callbacks")
{
	using ED = eventpp::EventDispatcher<int, void(int, std::string)>;
	ED dispatcher;
	std::vector<std::string> calls;
	dispatcher.appendListener(1, [&](int n, std::string s) { calls.push_back(s + std::to_string(n)); });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const int &, const std::string &) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, short(42), "converted");
		return plan;
	});
	dispatcher.dispatch(1, 0, "original");
	REQUIRE(calls == std::vector<std::string> {"converted42"});

	using EmptyED = eventpp::EventDispatcher<int, void()>;
	static_assert(! std::is_copy_constructible<EmptyED::ListenerPlan>::value, "Plans own their arguments.");
	EmptyED emptyDispatcher;
	int count = 0;
	emptyDispatcher.appendListener(1, [&] { ++count; });
	emptyDispatcher.setListenerPlanner(1, [](const EmptyED::ListenerList & listeners) {
		EmptyED::ListenerPlan plan;
		plan.add(listeners[0].handle);
		return plan;
	});
	emptyDispatcher.dispatch(1);
	REQUIRE(count == 1);
}

TEST_CASE("EventDispatcher planner, default calls do not copy abstract arguments")
{
	using ED = eventpp::EventDispatcher<int, void(const AbstractValue &)>;
	ED dispatcher;
	ConcreteValue value;
	int count = 0;
	dispatcher.appendListener(1, [&](const AbstractValue & arg) { REQUIRE(&arg == &value); count += arg.value(); });
	dispatcher.dispatch(1, value);
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const AbstractValue &) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle);
		return plan;
	});
	dispatcher.dispatch(1, value);
	REQUIRE(count == 14);
}

TEST_CASE("EventDispatcher planner, owned move only reference arguments")
{
	using ED = eventpp::EventDispatcher<int, void(std::unique_ptr<int> &)>;
	ED dispatcher;
	std::unique_ptr<int> original(new int(5));
	std::unique_ptr<int> received;
	dispatcher.appendListener(1, [&](std::unique_ptr<int> & arg) { received = std::move(arg); });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const std::unique_ptr<int> & arg) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, std::unique_ptr<int>(new int(*arg * 2)));
		return plan;
	});
	dispatcher.dispatch(1, original);
	REQUIRE(*original == 5);
	REQUIRE(*received == 10);
}

TEST_CASE("EventDispatcher planner, duplicate, foreign and new handles are ignored")
{
	using ED = eventpp::EventDispatcher<int, void(int)>;
	ED dispatcher, other;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](int n) { calls.push_back(n); });
	auto removed = dispatcher.appendListener(1, [&](int) { calls.push_back(-1); });
	auto foreignEvent = dispatcher.appendListener(2, [&](int) { calls.push_back(-2); });
	auto foreignDispatcher = other.appendListener(1, [&](int) { calls.push_back(-3); });
	dispatcher.setListenerPlanner(1, [&](const ED::ListenerList & listeners, const int &) {
		dispatcher.removeListener(1, removed);
		auto added = dispatcher.appendListener(1, [&](int n) { calls.push_back(n + 100); });
		ED::ListenerPlan plan;
		plan.add({}, 1);
		plan.add(removed, 2);
		plan.add(foreignEvent, 3);
		plan.add(foreignDispatcher, 4);
		plan.add(added, 5);
		plan.add(listeners[0].handle, 6);
		plan.add(listeners[0].handle, 7);
		return plan;
	});
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {6});
}

TEST_CASE("EventDispatcher planner, nested dispatch and configuration changes")
{
	using ED = eventpp::EventDispatcher<int, void(int)>;
	ED dispatcher;
	std::vector<int> calls;
	ED::Handle removed;
	dispatcher.appendListener(1, [&](int n) {
		calls.push_back(n);
		if(n == 10) {
			dispatcher.removeListener(1, removed);
			dispatcher.appendListener(1, [&](int value) { calls.push_back(100 + value); });
			dispatcher.clearListenerPlanner(1);
			dispatcher.dispatch(1, 2);
		}
	});
	removed = dispatcher.appendListener(1, [&](int) { calls.push_back(-1); });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const int & n) {
		ED::ListenerPlan plan;
		for(const auto & listener : listeners) { plan.add(listener.handle, n * 10); }
		return plan;
	});
	dispatcher.dispatch(1, 1);
	REQUIRE(calls == std::vector<int> {10, 2, 102});
}

TEST_CASE("EventDispatcher planner, replacement cancellation and filters")
{
	using ED = eventpp::EventDispatcher<int, void(CancelEvent &), CancelPolicies>;
	ED dispatcher;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](CancelEvent & event) { calls.push_back(1); event.canceled = true; });
	dispatcher.appendListener(1, [&](CancelEvent &) { calls.push_back(2); });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const CancelEvent &) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, CancelEvent {});
		plan.add(listeners[1].handle);
		return plan;
	});
	CancelEvent original;
	dispatcher.dispatch(1, original);
	REQUIRE_FALSE(original.canceled);
	REQUIRE(calls == std::vector<int> {1});

	using FilterED = eventpp::EventDispatcher<int, void(int &), FilterPolicies>;
	FilterED filtered;
	int result = 0;
	filtered.appendFilter([](int & n) { n += 1; return true; });
	filtered.appendListener(1, [&](int & n) { result = n; });
	filtered.setListenerPlanner(1, [](const FilterED::ListenerList & listeners, const int & n) {
		REQUIRE(n == 6);
		FilterED::ListenerPlan plan;
		plan.add(listeners[0].handle, n * 2);
		return plan;
	});
	int value = 5;
	filtered.dispatch(1, value);
	REQUIRE(value == 6);
	REQUIRE(result == 12);
}

TEST_CASE("EventDispatcher planner, ordering replacement, clearing and empty plans")
{
	using ED = eventpp::EventDispatcher<int, void(int)>;
	ED dispatcher;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](int n) { calls.push_back(n); });
	auto planner = [](const ED::ListenerList & listeners, const int &) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, 42);
		return plan;
	};
	dispatcher.setListenerPlanner(1, planner);
	dispatcher.dispatch(1, 1);
	dispatcher.setListenerOrdering(1, [](const ED::ListenerList & listeners, const int &) {
		return ED::ListenerOrder {listeners[0].handle};
	});
	dispatcher.dispatch(1, 2);
	dispatcher.setListenerPlanner(1, planner);
	dispatcher.clearListenerOrdering(1);
	dispatcher.dispatch(1, 3);
	dispatcher.setListenerPlanner(1, planner);
	dispatcher.setListenerPlanner(1, {});
	dispatcher.dispatch(1, 4);
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList &, const int &) {
		ED::ListenerPlan plan;
		REQUIRE(plan.empty());
		return plan;
	});
	dispatcher.dispatch(1, 5);
	REQUIRE(calls == std::vector<int> {42, 2, 3, 4});
}

TEST_CASE("EventDispatcher planner, copying, moving and swapping configuration")
{
	using ED = eventpp::EventDispatcher<int, void(int)>;
	ED dispatcher;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](int n) { calls.push_back(n); });
	dispatcher.setListenerPlanner(1, [count = 0](const ED::ListenerList & listeners, const int &) mutable {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, ++count);
		return plan;
	});
	dispatcher.dispatch(1, 0);
	ED copied(dispatcher), assigned;
	assigned = dispatcher;
	copied.dispatch(1, 0);
	assigned.dispatch(1, 0);
	dispatcher.dispatch(1, 0);
	ED moved(std::move(copied)), moveAssigned;
	moveAssigned = std::move(assigned);
	moved.dispatch(1, 0);
	moveAssigned.dispatch(1, 0);
	ED swapped;
	swap(swapped, moved);
	swapped.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1, 2, 2, 2, 3, 3, 4});
}

TEST_CASE("EventDispatcher planner, argument lifetime and exception cleanup")
{
	using ED = eventpp::EventDispatcher<int, void(const std::shared_ptr<int> &)>;
	ED dispatcher;
	std::weak_ptr<int> lifetime;
	int calls = 0;
	dispatcher.appendListener(1, [&](const std::shared_ptr<int> & arg) {
		++calls;
		REQUIRE(*arg == 42);
		REQUIRE_FALSE(lifetime.expired());
	});
	bool throwInPlanner = false;
	dispatcher.setListenerPlanner(1, [&](const ED::ListenerList & listeners, const std::shared_ptr<int> &) {
		ED::ListenerPlan plan;
		auto value = std::make_shared<int>(42);
		lifetime = value;
		plan.add(listeners[0].handle, value);
		if(throwInPlanner) { throw std::runtime_error("planner failed"); }
		return plan;
	});
	auto original = std::make_shared<int>(5);
	dispatcher.dispatch(1, original);
	REQUIRE(lifetime.expired());
	throwInPlanner = true;
	REQUIRE_THROWS_AS(dispatcher.dispatch(1, original), std::runtime_error);
	REQUIRE(calls == 1);
	REQUIRE(lifetime.expired());
	throwInPlanner = false;
	auto throwing = dispatcher.prependListener(1, [](const std::shared_ptr<int> &) { throw std::runtime_error("callback failed"); });
	REQUIRE_THROWS_AS(dispatcher.dispatch(1, original), std::runtime_error);
	REQUIRE(lifetime.expired());
	dispatcher.removeListener(1, throwing);
	dispatcher.dispatch(1, original);
	REQUIRE(calls == 2);
	REQUIRE(lifetime.expired());
}

TEST_CASE("EventQueue planner uses processing arguments and owns temporary replacements")
{
	using EQ = eventpp::EventQueue<int, void(int, const std::string &)>;
	EQ queue;
	std::vector<std::string> calls;
	queue.appendListener(1, [&](int n, const std::string & s) { calls.push_back(s + std::to_string(n)); });
	queue.enqueue(1, 2, "first");
	queue.enqueue(1, 3, "second");
	int plans = 0;
	queue.setListenerPlanner(1, [&](const EQ::ListenerList & listeners, const int & n, const std::string & s) {
		++plans;
		EQ::ListenerPlan plan;
		plan.add(listeners[0].handle, n * 10, s + "!");
		return plan;
	});
	REQUIRE(plans == 0);
	REQUIRE(queue.processOne());
	REQUIRE(queue.process());
	REQUIRE(plans == 2);
	REQUIRE(calls == std::vector<std::string> {"first!20", "second!30"});
}

TEST_CASE("EventDispatcher planner, concurrent dispatches have independent argument storage")
{
	using ED = eventpp::EventDispatcher<int, void(int &)>;
	ED dispatcher;
	std::atomic<int> total {0};
	dispatcher.appendListener(1, [&](int & n) { total += n; n = -1; });
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners, const int & n) {
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle, n * 2);
		return plan;
	});
	std::atomic<bool> originalChanged {false};
	auto dispatch = [&] {
		for(int i = 1; i <= 500; ++i) {
			int original = i;
			dispatcher.dispatch(1, original);
			if(original != i) { originalChanged = true; }
		}
	};
	std::thread first(dispatch), second(dispatch);
	first.join();
	second.join();
	REQUIRE(total == 501000);
	REQUIRE_FALSE(originalChanged);
}
