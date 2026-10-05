#include "test.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
struct Policies { using ListenerMetadata = std::map<std::string, int>; };
using Dispatcher = eventpp::EventDispatcher<int, void(), Policies>;
struct CopyOnlyMetadata {
	explicit CopyOnlyMetadata(int value = 0) : value(value) {}
	CopyOnlyMetadata(const CopyOnlyMetadata & other) : value(other.value) {
		if(failCopy) { throw std::runtime_error("copy failed"); }
	}
	CopyOnlyMetadata & operator = (const CopyOnlyMetadata &) = delete;
	static bool failCopy;
	int value;
};
bool CopyOnlyMetadata::failCopy = false;
struct CopyPolicies { using ListenerMetadata = CopyOnlyMetadata; };
}

TEST_CASE("Metadata update, default values, queue and standalone list")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [] {});
	REQUIRE(dispatcher.updateListenerMetadata(1, handle, [](Dispatcher::ListenerMetadata & metadata) {
		++metadata["count"];
	}));
	REQUIRE(dispatcher.getListeners(1)[0].metadata.at("count") == 1);
	REQUIRE(dispatcher.getListeners(1)[0].handle.lock() == handle.lock());
	eventpp::EventQueue<int, void(), Policies> queue;
	auto queued = queue.appendListener(1, [] {}, {{"count", 2}});
	REQUIRE(queue.updateListenerMetadata(1, queued, [](Dispatcher::ListenerMetadata & metadata) {
		++metadata["count"];
	}));
	REQUIRE(queue.getListeners(1)[0].metadata.at("count") == 3);
	eventpp::CallbackList<void(), Policies> list;
	auto listed = list.append([] {}, {{"count", 4}});
	REQUIRE(list.updateListenerMetadata(listed, [](Dispatcher::ListenerMetadata & metadata) {
		++metadata["count"];
	}));
	Dispatcher::ListenerMetadata output;
	REQUIRE(list.getListenerMetadata(listed, output));
	REQUIRE(output.at("count") == 5);
}

TEST_CASE("Metadata update, invalid and removed handles never invoke updater")
{
	Dispatcher dispatcher, other;
	auto handle = dispatcher.appendListener(1, [] {});
	auto foreign = other.appendListener(1, [] {});
	dispatcher.appendListener(2, [] {});
	int calls = 0;
	auto updater = [&](Dispatcher::ListenerMetadata &) { ++calls; };
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(1, {}, updater));
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(1, foreign, updater));
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(2, handle, updater));
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(999, handle, updater));
	auto pinned = handle.lock();
	REQUIRE(dispatcher.removeListener(1, handle));
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(1, handle, updater));
	pinned.reset();
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(1, handle, updater));
	REQUIRE(calls == 0);
}

TEST_CASE("Metadata update, conflicts retry on fresh values outside the mutex")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, {{"count", 1}});
	auto old = dispatcher.getListeners(1);
	int calls = 0;
	REQUIRE(dispatcher.updateListenerMetadata(1, handle, [&](Dispatcher::ListenerMetadata & metadata) {
		++calls;
		const int previous = metadata.at("count");
		++metadata["count"];
		REQUIRE(dispatcher.getListeners(1)[0].metadata.at("count") == previous);
		if(calls == 1) {
			REQUIRE(dispatcher.setListenerMetadata(1, handle, {{"count", 10}, {"extra", 7}}));
		}
	}));
	REQUIRE(calls == 2);
	auto current = dispatcher.getListeners(1);
	REQUIRE(current[0].metadata.at("count") == 11);
	REQUIRE(current[0].metadata.at("extra") == 7);
	REQUIRE(old[0].metadata.at("count") == 1);
}

TEST_CASE("Metadata update, removal during updater prevents commit")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, {{"count", 1}});
	REQUIRE_FALSE(dispatcher.updateListenerMetadata(1, handle, [&](Dispatcher::ListenerMetadata & metadata) {
		metadata["count"] = 99;
		REQUIRE(dispatcher.removeListener(1, handle));
	}));
	REQUIRE(dispatcher.getListeners(1).empty());
	REQUIRE(handle.expired());
}

TEST_CASE("Metadata update, exceptions commit nothing and copy assignment is not required")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, {{"count", 1}});
	REQUIRE_THROWS_AS(dispatcher.updateListenerMetadata(1, handle, [](Dispatcher::ListenerMetadata & metadata) {
		metadata["count"] = 99;
		throw std::runtime_error("update failed");
	}), std::runtime_error);
	REQUIRE(dispatcher.getListeners(1)[0].metadata.at("count") == 1);
	using CopyDispatcher = eventpp::EventDispatcher<int, void(), CopyPolicies>;
	CopyDispatcher copied;
	auto copyHandle = copied.appendListener(1, [] {}, CopyOnlyMetadata(7));
	int calls = 0;
	auto updater = [&](CopyOnlyMetadata & metadata) { ++calls; ++metadata.value; };
	CopyOnlyMetadata::failCopy = true;
	REQUIRE_THROWS_AS(copied.updateListenerMetadata(1, copyHandle, updater), std::runtime_error);
	CopyOnlyMetadata::failCopy = false;
	REQUIRE(calls == 0);
	REQUIRE(copied.getListeners(1)[0].metadata.value == 7);
	REQUIRE(copied.updateListenerMetadata(1, copyHandle, updater));
	REQUIRE(copied.getListeners(1)[0].metadata.value == 8);
}

TEST_CASE("Metadata update, move-only updaters and dispatcher copies")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, {{"count", 1}});
	Dispatcher copied(dispatcher);
	REQUIRE(dispatcher.updateListenerMetadata(1, handle,
		[increment = std::unique_ptr<int>(new int(3))](Dispatcher::ListenerMetadata & metadata) {
			metadata["count"] += *increment;
		}));
	REQUIRE(dispatcher.getListeners(1)[0].metadata.at("count") == 4);
	REQUIRE(copied.getListeners(1)[0].metadata.at("count") == 1);
}

TEST_CASE("Metadata update, concurrent increments do not lose updates or publish partial values")
{
	Dispatcher dispatcher;
	auto handle = dispatcher.appendListener(1, [] {}, {{"count", 0}, {"twice", 0}});
	std::atomic<bool> failed {false};
	std::vector<std::thread> writers;
	for(int thread = 0; thread < 4; ++thread) {
		writers.emplace_back([&] {
			for(int n = 0; n < 400; ++n) {
				if(! dispatcher.updateListenerMetadata(1, handle, [](Dispatcher::ListenerMetadata & metadata) {
					++metadata["count"];
					metadata["twice"] = metadata["count"] * 2;
				})) { failed = true; }
			}
		});
	}
	std::thread reader([&] {
		for(int n = 0; n < 1600; ++n) {
			auto listeners = dispatcher.getListeners(1);
			if(listeners[0].metadata.at("twice") != listeners[0].metadata.at("count") * 2) { failed = true; }
		}
	});
	for(auto & writer : writers) { writer.join(); }
	reader.join();
	REQUIRE_FALSE(failed);
	REQUIRE(dispatcher.getListeners(1)[0].metadata.at("count") == 1600);
}
