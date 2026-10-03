#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"

#include <atomic>
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
		for(int n = 0; n < 500; ++n) { dispatcher.dispatch(1, n); }
	});
	writer.join();
	reader.join();
	REQUIRE_FALSE(failed);
}
