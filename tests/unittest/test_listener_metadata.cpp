#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"

#include <atomic>
#include <functional>
#include <thread>
#include <stdexcept>

namespace {
struct MetadataPolicies {
	using ListenerMetadata = std::map<std::string, int>;
};
using Dispatcher = eventpp::EventDispatcher<int, int(int), MetadataPolicies>;

struct ThrowingMetadata {
	ThrowingMetadata() = default;
	explicit ThrowingMetadata(int value) : value(value) {}
	ThrowingMetadata(const ThrowingMetadata & other) : value(other.value) {
		if(throwOnCopy) { throw std::runtime_error("metadata copy failed"); }
	}
	ThrowingMetadata(ThrowingMetadata &&) = default;
	static bool throwOnCopy;
	int value = 0;
};
bool ThrowingMetadata::throwOnCopy = false;
struct ThrowPolicies {
	using ListenerMetadata = ThrowingMetadata;
};

struct AssignableMetadata {
	int value = 0;
	std::function<void()> onAssign;
	AssignableMetadata & operator = (const AssignableMetadata & other) {
		if(onAssign) { onAssign(); }
		value = other.value;
		return *this;
	}
};
struct AssignPolicies {
	using ListenerMetadata = AssignableMetadata;
};
}

TEST_CASE("Listener snapshot, list order, event isolation and default metadata")
{
	Dispatcher dispatcher;
	auto a = dispatcher.appendListener(1, [](int n) { return n; }, {{"id", 1}});
	auto c = dispatcher.appendListener(1, [](int n) { return n; });
	auto b = dispatcher.prependListener(1, [](int n) { return n; }, {{"id", 2}});
	auto inserted = dispatcher.insertListener(1, [](int n) { return n; }, c, {{"id", 3}});
	auto other = dispatcher.appendListener(2, [](int n) { return n; }, {{"id", 4}});
	REQUIRE(dispatcher.setListenerMetadata(1, a, {{"id", 10}}));
	const Dispatcher & view = dispatcher;
	auto listeners = view.getListeners(1);
	REQUIRE(listeners.size() == 4);
	REQUIRE(listeners[0].handle.lock() == b.lock());
	REQUIRE(listeners[1].handle.lock() == a.lock());
	REQUIRE(listeners[2].handle.lock() == inserted.lock());
	REQUIRE(listeners[3].handle.lock() == c.lock());
	REQUIRE(listeners[0].metadata.at("id") == 2);
	REQUIRE(listeners[1].metadata.at("id") == 10);
	REQUIRE(listeners[2].metadata.at("id") == 3);
	REQUIRE(listeners[3].metadata.empty());
	auto second = view.getListeners(2);
	REQUIRE(second.size() == 1);
	REQUIRE(second[0].handle.lock() == other.lock());
	REQUIRE(view.getListeners(999).empty());
	REQUIRE(dispatcher.removeListener(2, other));
	REQUIRE(view.getListeners(2).empty());

	eventpp::EventDispatcher<int, void()> defaults;
	defaults.appendListener(1, [] {}, {{"name", "first"}});
	REQUIRE(defaults.getListeners(1)[0].metadata.at("name") == "first");
}

TEST_CASE("Listener snapshot, independent values and weak subscription lifetime")
{
	Dispatcher dispatcher;
	auto a = dispatcher.appendListener(1, [](int n) { return n; }, {{"id", 1}});
	auto b = dispatcher.appendListener(1, [](int n) { return n; }, {{"id", 2}});
	auto original = dispatcher.getListeners(1);
	auto local = original;
	local[0].metadata["id"] = 99;
	local.clear();
	REQUIRE(dispatcher.getListeners(1)[0].metadata.at("id") == 1);
	REQUIRE(dispatcher.setListenerMetadata(1, a, {{"id", 10}}));
	auto pinned = b.lock();
	REQUIRE(dispatcher.removeListener(1, b));
	auto added = dispatcher.appendListener(1, [](int n) { return n; }, {{"id", 3}});
	auto current = dispatcher.getListeners(1);
	REQUIRE(current.size() == 2);
	REQUIRE(current[0].metadata.at("id") == 10);
	REQUIRE(current[1].handle.lock() == added.lock());
	REQUIRE(original.size() == 2);
	REQUIRE(original[0].metadata.at("id") == 1);
	REQUIRE(original[1].metadata.at("id") == 2);
	pinned.reset();
	REQUIRE(original[1].handle.expired());
}

TEST_CASE("Listener snapshot, queue inspection does not execute selection, handlers or aggregation")
{
	using Queue = eventpp::EventQueue<int, int(int), MetadataPolicies>;
	Queue queue;
	int selections = 0, calls = 0, aggregates = 0;
	queue.appendListener(1, [&](int n) { ++calls; return n; }, {{"id", 1}});
	queue.appendListener(1, [&](int n) { ++calls; return n; }, {{"id", 2}});
	queue.setListenerOrdering(1, [&](const Queue::ListenerList &, const int &) {
		++selections;
		return Queue::ListenerOrder {};
	});
	queue.setResultAggregator(1, [&](const Queue::ListenerResults &) { ++aggregates; return 0; });
	auto future = queue.enqueueWithResults(1, 5);
	const Queue & view = queue;
	auto listeners = view.getListeners(1);
	REQUIRE(listeners.size() == 2);
	REQUIRE(listeners[0].metadata.at("id") == 1);
	REQUIRE(listeners[1].metadata.at("id") == 2);
	REQUIRE(selections == 0);
	REQUIRE(calls == 0);
	REQUIRE(aggregates == 0);
	REQUIRE(queue.process());
	REQUIRE(future.get().results.empty());
	REQUIRE(selections == 1);
	REQUIRE(aggregates == 1);
	REQUIRE(calls == 0);
}

TEST_CASE("Listener snapshot, metadata copy exceptions leave subscriptions intact")
{
	using ED = eventpp::EventDispatcher<int, void(), ThrowPolicies>;
	ED dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, ThrowingMetadata(7));
	ThrowingMetadata::throwOnCopy = true;
	REQUIRE_THROWS_AS(dispatcher.getListeners(1), std::runtime_error);
	ThrowingMetadata::throwOnCopy = false;
	auto listeners = dispatcher.getListeners(1);
	REQUIRE(listeners.size() == 1);
	REQUIRE(listeners[0].metadata.value == 7);
	REQUIRE(listeners[0].handle.lock() == handle.lock());
	REQUIRE(dispatcher.setListenerMetadata(1, handle, ThrowingMetadata(8)));
	REQUIRE(dispatcher.getListeners(1)[0].metadata.value == 8);
}

TEST_CASE("Listener snapshot, concurrent subscription and metadata changes remain consistent")
{
	Dispatcher dispatcher;
	auto fixed = dispatcher.appendListener(1, [](int n) { return n; }, {{"a", 0}, {"b", 0}});
	std::atomic<bool> failed {false};
	std::thread writer([&] {
		for(int n = 1; n <= 500; ++n) {
			if(! dispatcher.setListenerMetadata(1, fixed, {{"a", n}, {"b", n}})) { failed = true; }
			auto temporary = dispatcher.appendListener(1, [](int value) { return value; }, {{"a", n}, {"b", n}});
			dispatcher.removeListener(1, temporary);
		}
	});
	std::thread reader([&] {
		for(int n = 0; n < 500; ++n) {
			auto listeners = dispatcher.getListeners(1);
			if(listeners.empty() || listeners[0].handle.lock() != fixed.lock()) { failed = true; }
			for(const auto & listener : listeners) {
				if(listener.metadata.at("a") != listener.metadata.at("b")) { failed = true; }
			}
		}
	});
	writer.join();
	reader.join();
	REQUIRE_FALSE(failed);
}

TEST_CASE("Listener metadata, getters return independent copies and default values")
{
	using ED = eventpp::EventDispatcher<int, void()>;
	ED dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, {{"name", "original"}});
	const ED & view = dispatcher;
	ED::ListenerMetadata metadata;
	REQUIRE(view.getListenerMetadata(1, handle, metadata));
	REQUIRE(metadata.at("name") == "original");
	metadata["name"] = "local";
	REQUIRE(view.getListenerMetadata(1, handle, metadata));
	REQUIRE(metadata.at("name") == "original");
	REQUIRE(dispatcher.setListenerMetadata(1, handle, {{"name", "updated"}}));
	REQUIRE(view.getListenerMetadata(1, handle, metadata));
	REQUIRE(metadata.at("name") == "updated");
	auto emptyHandle = dispatcher.appendListener(1, [] {});
	REQUIRE(view.getListenerMetadata(1, emptyHandle, metadata));
	REQUIRE(metadata.empty());

	eventpp::EventQueue<int, void(), MetadataPolicies> queue;
	auto queueHandle = queue.appendListener(1, [] {}, {{"value", 7}});
	const auto & queueView = queue;
	Dispatcher::ListenerMetadata custom;
	REQUIRE(queueView.getListenerMetadata(1, queueHandle, custom));
	REQUIRE(custom.at("value") == 7);

	eventpp::CallbackList<void(), MetadataPolicies> list;
	auto listHandle = list.append([] {}, {{"value", 8}});
	const auto & listView = list;
	REQUIRE(listView.getListenerMetadata(listHandle, custom));
	REQUIRE(custom.at("value") == 8);
}

TEST_CASE("Listener metadata, failed getters leave the output unchanged")
{
	Dispatcher dispatcher, other;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; });
	auto foreign = other.appendListener(1, [](int n) { return n; });
	dispatcher.appendListener(2, [](int n) { return n; });
	Dispatcher::ListenerMetadata metadata {{"keep", 42}};
	REQUIRE_FALSE(dispatcher.getListenerMetadata(1, {}, metadata));
	REQUIRE_FALSE(dispatcher.getListenerMetadata(1, foreign, metadata));
	REQUIRE_FALSE(dispatcher.getListenerMetadata(2, handle, metadata));
	REQUIRE_FALSE(dispatcher.getListenerMetadata(999, handle, metadata));
	REQUIRE(metadata == Dispatcher::ListenerMetadata {{"keep", 42}});
	auto pinned = handle.lock();
	REQUIRE(dispatcher.removeListener(1, handle));
	REQUIRE_FALSE(dispatcher.getListenerMetadata(1, handle, metadata));
	pinned.reset();
	REQUIRE_FALSE(dispatcher.getListenerMetadata(1, handle, metadata));
	REQUIRE(metadata == Dispatcher::ListenerMetadata {{"keep", 42}});

	eventpp::CallbackList<void(), MetadataPolicies> list, otherList;
	auto listHandle = list.append([] {}, {{"value", 1}});
	REQUIRE_FALSE(otherList.getListenerMetadata(listHandle, metadata));
	REQUIRE(metadata == Dispatcher::ListenerMetadata {{"keep", 42}});
}

TEST_CASE("Listener metadata, getters see updates made during a dispatch snapshot")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; }, {{"value", 1}});
	dispatcher.setListenerPlanner(1, [&](const Dispatcher::ListenerList & listeners, const int &) {
		REQUIRE(dispatcher.setListenerMetadata(1, handle, {{"value", 2}}));
		Dispatcher::ListenerMetadata metadata;
		REQUIRE(dispatcher.getListenerMetadata(1, handle, metadata));
		REQUIRE(metadata.at("value") == 2);
		REQUIRE(listeners[0].metadata.at("value") == 1);
		return Dispatcher::ListenerPlan();
	});
	dispatcher.dispatch(1, 0);
}

TEST_CASE("Listener metadata, getter assignment can reenter and exceptions preserve stored metadata")
{
	using ED = eventpp::EventDispatcher<int, void(), AssignPolicies>;
	ED dispatcher;
	AssignableMetadata initial;
	initial.value = 1;
	auto handle = dispatcher.appendListener(1, [] {}, initial);
	AssignableMetadata output;
	output.onAssign = [&] {
		AssignableMetadata replacement;
		replacement.value = 2;
		REQUIRE(dispatcher.setListenerMetadata(1, handle, replacement));
	};
	REQUIRE(dispatcher.getListenerMetadata(1, handle, output));
	REQUIRE(output.value == 1);
	output.onAssign = [] { throw std::runtime_error("metadata assignment failed"); };
	REQUIRE_THROWS_AS(dispatcher.getListenerMetadata(1, handle, output), std::runtime_error);
	output.onAssign = nullptr;
	REQUIRE(dispatcher.getListenerMetadata(1, handle, output));
	REQUIRE(output.value == 2);
	REQUIRE(dispatcher.removeListener(1, handle));
	output.onAssign = [] { throw std::runtime_error("must not assign for a removed listener"); };
	REQUIRE_FALSE(dispatcher.getListenerMetadata(1, handle, output));
}

TEST_CASE("Listener metadata, updates replace values and preserve subscription identity")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; }, {{"factor", 1}, {"old", 1}});
	REQUIRE(dispatcher.setListenerMetadata(1, handle, {{"factor", 3}}));
	REQUIRE(dispatcher.ownsHandle(1, handle));
	dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int & n) {
		REQUIRE(listeners[0].metadata.count("old") == 0);
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[0].handle, n * listeners[0].metadata.at("factor"));
		return plan;
	});
	REQUIRE(dispatcher.dispatchWithResults(1, 2).results == std::vector<int> {6});
}

TEST_CASE("Listener metadata, invalid, foreign and removed handles are rejected")
{
	Dispatcher dispatcher, other;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; });
	auto foreign = other.appendListener(1, [](int n) { return n; });
	dispatcher.appendListener(2, [](int n) { return n; });
	REQUIRE_FALSE(dispatcher.setListenerMetadata(1, {}, {}));
	REQUIRE_FALSE(dispatcher.setListenerMetadata(1, foreign, {}));
	REQUIRE_FALSE(dispatcher.setListenerMetadata(2, handle, {}));
	REQUIRE_FALSE(dispatcher.setListenerMetadata(999, handle, {}));
	auto pinned = handle.lock();
	REQUIRE(dispatcher.removeListener(1, handle));
	REQUIRE_FALSE(dispatcher.setListenerMetadata(1, handle, {}));
	pinned.reset();
	REQUIRE_FALSE(dispatcher.setListenerMetadata(1, handle, {}));
}

TEST_CASE("Listener metadata, current snapshot is stable and nested dispatch sees update")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; }, {{"factor", 1}});
	std::vector<int> observed;
	dispatcher.setListenerPlanner(1, [&](const Dispatcher::ListenerList & listeners, const int & depth) {
		const int factor = listeners[0].metadata.at("factor");
		observed.push_back(factor);
		if(depth == 0) {
			REQUIRE(dispatcher.setListenerMetadata(1, handle, {{"factor", 2}}));
			REQUIRE(dispatcher.dispatchWithResults(1, 1).results == std::vector<int> {2});
			REQUIRE(listeners[0].metadata.at("factor") == 1);
		}
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[0].handle, factor);
		return plan;
	});
	REQUIRE(dispatcher.dispatchWithResults(1, 0).results == std::vector<int> {1});
	REQUIRE(dispatcher.dispatchWithResults(1, 1).results == std::vector<int> {2});
	REQUIRE(observed == std::vector<int> {1, 2, 2});
}

TEST_CASE("Listener metadata, copies and standalone callback lists remain independent")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; }, {{"factor", 1}});
	dispatcher.setListenerPlanner(1, [](const Dispatcher::ListenerList & listeners, const int & n) {
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[0].handle, n * listeners[0].metadata.at("factor"));
		return plan;
	});
	Dispatcher copied(dispatcher);
	REQUIRE(dispatcher.setListenerMetadata(1, handle, {{"factor", 2}}));
	REQUIRE(dispatcher.dispatchWithResults(1, 3).results == std::vector<int> {6});
	REQUIRE(copied.dispatchWithResults(1, 3).results == std::vector<int> {3});
	Dispatcher::Handle copiedHandle;
	copied.forEach(1, [&](const Dispatcher::Handle & h, Dispatcher::Callback &) { copiedHandle = h; });
	REQUIRE(copied.setListenerMetadata(1, copiedHandle, {{"factor", 4}}));
	REQUIRE(copied.dispatchWithResults(1, 3).results == std::vector<int> {12});
	REQUIRE_FALSE(copied.setListenerMetadata(1, handle, {}));

	eventpp::CallbackList<void(), MetadataPolicies> list, other;
	auto listHandle = list.append([] {}, {{"factor", 1}});
	REQUIRE(list.setListenerMetadata(listHandle, {{"factor", 2}}));
	REQUIRE_FALSE(other.setListenerMetadata(listHandle, {}));
}

TEST_CASE("Listener metadata, copy failure leaves the previous value intact")
{
	using ED = eventpp::EventDispatcher<int, int(), ThrowPolicies>;
	ED dispatcher;
	auto handle = dispatcher.appendListener(1, [] { return 7; }, ThrowingMetadata(1));
	ThrowingMetadata replacement(2);
	ThrowingMetadata::throwOnCopy = true;
	REQUIRE_THROWS_AS(dispatcher.setListenerMetadata(1, handle, replacement), std::runtime_error);
	ThrowingMetadata::throwOnCopy = false;
	dispatcher.setListenerPlanner(1, [](const ED::ListenerList & listeners) {
		REQUIRE(listeners[0].metadata.value == 1);
		ED::ListenerPlan plan;
		plan.add(listeners[0].handle);
		return plan;
	});
	REQUIRE(dispatcher.dispatchWithResults(1).results == std::vector<int> {7});
}

TEST_CASE("Listener metadata, concurrent updates produce consistent snapshots")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [](int n) { return n; }, {{"a", 0}, {"b", 0}});
	std::atomic<bool> failed {false};
	dispatcher.setListenerPlanner(1, [&](const Dispatcher::ListenerList & listeners, const int &) {
		if(listeners[0].metadata.at("a") != listeners[0].metadata.at("b")) { failed = true; }
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[0].handle);
		return plan;
	});
	std::thread writer([&] {
		for(int n = 1; n <= 500; ++n) {
			if(! dispatcher.setListenerMetadata(1, handle, {{"a", n}, {"b", n}})) { failed = true; }
		}
	});
	std::thread reader([&] {
		for(int n = 0; n < 500; ++n) {
			dispatcher.dispatch(1, n);
			Dispatcher::ListenerMetadata metadata;
			if(! dispatcher.getListenerMetadata(1, handle, metadata)
				|| metadata.at("a") != metadata.at("b")) { failed = true; }
		}
	});
	writer.join();
	reader.join();
	REQUIRE_FALSE(failed);
}
