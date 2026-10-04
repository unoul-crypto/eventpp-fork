#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"
#include "eventpp/mixins/mixinfilter.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace {
using Dispatcher = eventpp::EventDispatcher<int, void(int)>;

Dispatcher::ListenerOrder inListOrder(const Dispatcher::ListenerList & listeners)
{
	Dispatcher::ListenerOrder order;
	for(const auto & listener : listeners) {
		order.push_back(listener.handle);
	}
	return order;
}

struct NumericMetadataPolicies {
	using ListenerMetadata = std::unordered_map<std::string, int>;
};

struct CancelEvent {
	bool canceled = false;
};

struct CancelPolicies {
	using Mixins = eventpp::MixinList<eventpp::MixinFilter>;
	static int getEvent(const CancelEvent &) { return 1; }
	static bool canContinueInvoking(const CancelEvent & event) { return ! event.canceled; }
};
}

TEST_CASE("EventDispatcher ordering, metadata, arguments and selection")
{
	Dispatcher dispatcher;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](int n) { calls.push_back(n + 1); }, {{"name", "first"}});
	dispatcher.appendListener(1, [&](int n) { calls.push_back(n + 2); }, {{"name", "second"}});
	dispatcher.appendListener(1, [&](int n) { calls.push_back(n + 3); }, {{"name", "skip"}});
	dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList & listeners, const int & n) {
		REQUIRE(listeners.size() == 3);
		REQUIRE(listeners[0].metadata.at("name") == "first");
		REQUIRE(listeners[1].metadata.at("name") == "second");
		return n > 0 ? Dispatcher::ListenerOrder { listeners[1].handle, listeners[0].handle }
			: Dispatcher::ListenerOrder {};
	});
	dispatcher.dispatch(1, 10);
	REQUIRE(calls == std::vector<int> {12, 11});
	calls.clear();
	dispatcher.dispatch(1, 0);
	REQUIRE(calls.empty());
	dispatcher.clearListenerOrdering(1);
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1, 2, 3});
}

TEST_CASE("EventDispatcher ordering, list positions and custom metadata")
{
	using ED = eventpp::EventDispatcher<int, void(), NumericMetadataPolicies>;
	ED dispatcher;
	std::vector<int> calls;
	auto a = dispatcher.appendListener(1, [&] { calls.push_back(1); }, {{"priority", 1}});
	dispatcher.prependListener(1, [&] { calls.push_back(2); }, {{"priority", 2}});
	dispatcher.insertListener(1, [&] { calls.push_back(3); }, a, {{"priority", 3}});
	dispatcher.appendListener(1, [&] { calls.push_back(4); });
	dispatcher.dispatch(1);
	REQUIRE(calls == std::vector<int> {2, 3, 1, 4});
	calls.clear();
	dispatcher.setListenerOrdering(1, [](const ED::ListenerList & listeners) {
		REQUIRE(listeners[3].metadata.empty());
		auto sorted = listeners;
		std::stable_sort(sorted.begin(), sorted.end(), [](const ED::ListenerInfo & a, const ED::ListenerInfo & b) {
			const int pa = a.metadata.empty() ? 0 : a.metadata.at("priority");
			const int pb = b.metadata.empty() ? 0 : b.metadata.at("priority");
			return pa > pb;
		});
		ED::ListenerOrder order;
		for(const auto & listener : sorted) { order.push_back(listener.handle); }
		return order;
	});
	dispatcher.dispatch(1);
	REQUIRE(calls == std::vector<int> {3, 2, 1, 4});
}

TEST_CASE("EventDispatcher ordering, event isolation and empty function reset")
{
	Dispatcher dispatcher;
	std::vector<int> calls;
	dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList &, const int &) {
		return Dispatcher::ListenerOrder {};
	});
	dispatcher.appendListener(1, [&](int) { calls.push_back(1); });
	dispatcher.appendListener(2, [&](int) { calls.push_back(2); });
	dispatcher.dispatch(1, 0);
	dispatcher.dispatch(2, 0);
	REQUIRE(calls == std::vector<int> {2});
	dispatcher.setListenerOrdering(1, {});
	dispatcher.clearListenerOrdering(99);
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {2, 1});
}

TEST_CASE("EventDispatcher ordering, ignore duplicate, foreign and expired handles")
{
	Dispatcher dispatcher, other;
	std::vector<int> calls;
	auto a = dispatcher.appendListener(1, [&](int) { calls.push_back(1); });
	auto b = dispatcher.appendListener(2, [&](int) { calls.push_back(2); });
	auto foreign = other.appendListener(1, [&](int) { calls.push_back(3); });
	auto removed = dispatcher.appendListener(1, [&](int) { calls.push_back(4); });
	dispatcher.removeListener(1, removed);
	dispatcher.setListenerOrdering(1, [=](const Dispatcher::ListenerList &, const int &) {
		return Dispatcher::ListenerOrder { {}, removed, b, foreign, a, a };
	});
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1});
}

TEST_CASE("EventDispatcher ordering, handle validation preserves control block identity")
{
	Dispatcher dispatcher;
	std::vector<int> calls;
	auto a = dispatcher.appendListener(1, [&](int) { calls.push_back(1); });
	auto b = dispatcher.appendListener(1, [&](int) { calls.push_back(2); });
	auto pinned = a.lock();
	using NodePtr = decltype(pinned);
	// Same pointer, different owner: this must not be accepted as the subscription.
	NodePtr borrowed(pinned.get(), [](NodePtr::element_type *) {});
	Dispatcher::Handle foreignOwner(borrowed);
	// Aliasing the same owner and pointer is a valid copy of the handle.
	NodePtr alias(pinned, pinned.get());
	Dispatcher::Handle sameOwner(alias);
	dispatcher.setListenerOrdering(1, [=](const Dispatcher::ListenerList &, const int &) {
		return Dispatcher::ListenerOrder {foreignOwner, sameOwner, b, b, a};
	});
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1, 2});
	calls.clear();
	REQUIRE(dispatcher.removeListener(1, a));
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {2});
	pinned.reset();
	alias.reset();
	calls.clear();
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {2});
}

TEST_CASE("EventDispatcher ordering, mutations during ordering use a snapshot")
{
	Dispatcher dispatcher;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](int) { calls.push_back(1); });
	auto removed = dispatcher.appendListener(1, [&](int) { calls.push_back(2); });
	dispatcher.setListenerOrdering(1, [&](const Dispatcher::ListenerList & listeners, const int &) {
		dispatcher.removeListener(1, removed);
		auto added = dispatcher.appendListener(1, [&](int) { calls.push_back(3); });
		dispatcher.clearListenerOrdering(1);
		auto order = inListOrder(listeners);
		order.push_back(added);
		return order;
	});
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1});
	calls.clear();
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1, 3});
}

TEST_CASE("EventDispatcher ordering, removal and nested dispatch in listeners")
{
	Dispatcher dispatcher;
	std::vector<int> calls;
	Dispatcher::Handle second;
	dispatcher.appendListener(1, [&](int depth) {
		calls.push_back(10 + depth);
		if(depth == 0) {
			dispatcher.removeListener(1, second);
			dispatcher.appendListener(1, [&](int n) { calls.push_back(30 + n); });
			dispatcher.dispatch(1, 1);
		}
	});
	second = dispatcher.appendListener(1, [&](int n) { calls.push_back(20 + n); });
	dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList & listeners, const int &) {
		return inListOrder(listeners);
	});
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {10, 11, 31});
}

TEST_CASE("EventDispatcher ordering, copy, assignment, move and swap")
{
	Dispatcher dispatcher;
	std::vector<int> calls;
	dispatcher.appendListener(1, [&](int) { calls.push_back(1); }, {{"id", "one"}});
	dispatcher.appendListener(1, [&](int) { calls.push_back(2); }, {{"id", "two"}});
	dispatcher.setListenerOrdering(1, [count = 0](const Dispatcher::ListenerList & listeners, const int &) mutable {
		REQUIRE(listeners[1].metadata.at("id") == "two");
		return Dispatcher::ListenerOrder { listeners[(count++) % 2].handle };
	});
	dispatcher.dispatch(1, 0); // Advance function state before copying.
	Dispatcher copied(dispatcher), assigned;
	assigned = dispatcher;
	copied.dispatch(1, 0);
	assigned.dispatch(1, 0);
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1, 2, 2, 2});
	Dispatcher moved(std::move(copied)), moveAssigned;
	moveAssigned = std::move(assigned);
	moved.dispatch(1, 0);
	moveAssigned.dispatch(1, 0);
	Dispatcher swapped;
	swap(swapped, moved);
	swapped.dispatch(1, 0);
	REQUIRE(calls == std::vector<int> {1, 2, 2, 2, 1, 1, 2});
}

TEST_CASE("EventQueue ordering observes arguments at processing time")
{
	using EQ = eventpp::EventQueue<int, void(std::string)>;
	EQ queue;
	std::vector<std::string> calls;
	queue.appendListener(1, [&](std::string s) { calls.push_back("A" + s); }, {{"id", "A"}});
	queue.appendListener(1, [&](std::string s) { calls.push_back("B" + s); }, {{"id", "B"}});
	queue.enqueue(1, "first");
	queue.enqueue(1, "second");
	int orderingCalls = 0;
	queue.setListenerOrdering(1, [&](const EQ::ListenerList & listeners, const std::string & value) {
		++orderingCalls;
		REQUIRE(listeners[1].metadata.at("id") == "B");
		return value == "first" ? EQ::ListenerOrder {listeners[1].handle, listeners[0].handle}
			: EQ::ListenerOrder {listeners[0].handle};
	});
	REQUIRE(calls.empty());
	REQUIRE(orderingCalls == 0);
	REQUIRE(queue.processOne());
	REQUIRE(queue.process());
	REQUIRE(orderingCalls == 2);
	REQUIRE(calls == std::vector<std::string> {"Bfirst", "Afirst", "Asecond"});
}

TEST_CASE("EventDispatcher ordering preserves reference arguments, filters and cancellation")
{
	using ED = eventpp::EventDispatcher<int, void(CancelEvent &), CancelPolicies>;
	ED dispatcher;
	std::vector<int> calls;
	int orderingCalls = 0;
	dispatcher.appendFilter([](CancelEvent & event) { return ! event.canceled; });
	dispatcher.appendListener(1, [&](CancelEvent &) { calls.push_back(1); });
	dispatcher.appendListener(1, [&](CancelEvent & event) { calls.push_back(2); event.canceled = true; });
	dispatcher.setListenerOrdering(1, [&](const ED::ListenerList & listeners, const CancelEvent & event) {
		++orderingCalls;
		REQUIRE_FALSE(event.canceled);
		return ED::ListenerOrder {listeners[1].handle, listeners[0].handle};
	});
	CancelEvent event;
	dispatcher.dispatch(event);
	REQUIRE(event.canceled);
	REQUIRE(calls == std::vector<int> {2});
	dispatcher.dispatch(event);
	REQUIRE(orderingCalls == 1);
}

TEST_CASE("EventDispatcher ordering, exceptions leave dispatcher usable")
{
	Dispatcher dispatcher;
	int calls = 0;
	dispatcher.appendListener(1, [&](int) { ++calls; });
	dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList &, const int &) -> Dispatcher::ListenerOrder {
		throw std::runtime_error("ordering failed");
	});
	REQUIRE_THROWS_AS(dispatcher.dispatch(1, 0), std::runtime_error);
	REQUIRE(calls == 0);
	dispatcher.clearListenerOrdering(1);
	dispatcher.dispatch(1, 0);
	REQUIRE(calls == 1);
}

TEST_CASE("EventDispatcher ordering, concurrent subscription and configuration")
{
	Dispatcher dispatcher;
	std::atomic<int> calls {0};
	dispatcher.appendListener(1, [&](int) { ++calls; }, {{"id", "fixed"}});
	auto ordering = [](const Dispatcher::ListenerList & listeners, const int &) { return inListOrder(listeners); };
	dispatcher.setListenerOrdering(1, ordering);
	std::thread producer([&] {
		for(int i = 0; i < 500; ++i) {
			auto handle = dispatcher.appendListener(1, [](int) {}, {{"id", "temporary"}});
			dispatcher.removeListener(1, handle);
			dispatcher.setListenerOrdering(1, ordering);
		}
	});
	std::thread consumer([&] {
		for(int i = 0; i < 500; ++i) { dispatcher.dispatch(1, i); }
	});
	producer.join();
	consumer.join();
	REQUIRE(calls == 500);
}
