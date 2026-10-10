#include "test.h"
#include "test_threading.h"
#include "eventpp/eventqueue.h"
#include "eventpp/utilities/orderedqueuelist.h"

#include <atomic>
#include <thread>

namespace {
using Queue = eventpp::EventQueue<int, int(int)>;
struct SinglePolicies { using Threading = eventpp::SingleThreading; };
struct OrderedPolicies { template <typename T> using QueueList = eventpp::OrderedQueueList<T>; };
struct Event { int type; int value; };
struct EventPolicies { static int getEvent(const Event & event) { return event.type; } };
struct PointerPolicies { using AggregationResult = int; };
}

TEST_CASE("Queue task cancellation is immediate, individual and terminal")
{
	Queue queue;
	int calls = 0;
	queue.appendListener(1, [&](int n) { ++calls; return n; });
	auto cancelled = queue.enqueueTask(1, 10);
	auto executed = queue.enqueueTask(1, 20);
	REQUIRE(cancelled.cancel());
	REQUIRE_FALSE(cancelled.cancel());
	REQUIRE(cancelled.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
	REQUIRE_THROWS_AS(cancelled.future.get(), eventpp::QueuedEventCancelled);
	REQUIRE(queue.process());
	REQUIRE(executed.future.get().results == std::vector<int> {20});
	REQUIRE_FALSE(executed.cancel());
	REQUIRE(calls == 1);
	REQUIRE(queue.emptyQueue());
	Queue::QueuedTask empty;
	REQUIRE_FALSE(empty.cancel());
}

TEST_CASE("Queue tasks preserve legacy cancellation and complete on abandonment")
{
	SECTION("Clear") {
		Queue queue;
		auto legacy = queue.enqueueWithResults(1, 1);
		auto task = queue.enqueueTask(1, 2);
		queue.clearEvents();
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
		REQUIRE_FALSE(task.cancel());
		try { legacy.get(); FAIL("Expected broken promise"); }
		catch(const std::future_error & error) { REQUIRE(error.code() == std::make_error_code(std::future_errc::broken_promise)); }
	}
	SECTION("Destruction") {
		Queue::QueuedTask task;
		{
			Queue queue;
			task = queue.enqueueTask(1, 2);
		}
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
		REQUIRE_FALSE(task.cancel());
	}
	SECTION("Abandoned taken record") {
		Queue queue;
		auto task = queue.enqueueTask(1, 2);
		{
			Queue::QueuedEvent record;
			REQUIRE(queue.takeEvent(&record));
		}
		REQUIRE(task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
	}
	SECTION("Discarding a task does not cancel execution") {
		Queue queue;
		int calls = 0;
		queue.appendListener(1, [&](int n) { ++calls; return n; });
		queue.enqueueTask(1, 2);
		queue.process();
		REQUIRE(calls == 1);
	}
	SECTION("Abandoned batch") {
		Queue queue;
		queue.appendListener(2, [](int) -> int { throw std::runtime_error("ordinary event"); });
		queue.enqueue(2, 0);
		auto task = queue.enqueueTask(1, 2);
		REQUIRE_THROWS_WITH(queue.process(), "ordinary event");
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
	}
}

TEST_CASE("Canceled task records are reclaimed in all queue processing modes")
{
	for(int mode = 0; mode < 4; ++mode) {
		Queue queue;
		int selections = 0;
		int predicates = 0;
		queue.appendListener(1, [](int n) { return n; });
		queue.setListenerPlanner(1, [&](const Queue::ListenerList &, const int &) { ++selections; return Queue::ListenerPlan(); });
		queue.setResultAggregator(1, [&](const Queue::ListenerResults &) { ++selections; return 0; });
		auto task = queue.enqueueTask(1, 2);
		REQUIRE(task.cancel());
		switch(mode) {
		case 0: REQUIRE(queue.process()); break;
		case 1: REQUIRE(queue.processOne()); break;
		case 2: REQUIRE(queue.processIf([&](int) { ++predicates; return false; })); break;
		case 3: REQUIRE(queue.processUntil([&](int) { ++predicates; return true; })); break;
		}
		REQUIRE(queue.emptyQueue());
		REQUIRE(selections == 0);
		REQUIRE(predicates == 0);
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
	}
}

TEST_CASE("Queue tasks, saved copies share cancellation and survive queue lifetime when taken")
{
	SECTION("Peek copies") {
		Queue queue;
		int calls = 0;
		queue.appendListener(1, [&](int n) { ++calls; return n; });
		auto task = queue.enqueueTask(1, 3);
		Queue::QueuedEvent record;
		REQUIRE(queue.peekEvent(&record));
		REQUIRE(task.cancel());
		queue.dispatch(record);
		queue.process();
		REQUIRE(calls == 0);
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
	}
	SECTION("Taken work") {
		Queue::QueuedTask task;
		Queue::QueuedEvent record;
		{
			Queue queue;
			task = queue.enqueueTask(1, 3);
			REQUIRE(queue.takeEvent(&record));
		}
		REQUIRE(task.cancel());
		REQUIRE_THROWS_AS(task.future.get(), eventpp::QueuedEventCancelled);
	}
}

TEST_CASE("Queue tasks support event extraction, void reports, move-only values and policies")
{
	SECTION("Extracted event") {
		eventpp::EventQueue<int, int(const Event &), EventPolicies> queue;
		queue.appendListener(1, [](const Event & event) { return event.value; });
		auto task = queue.enqueueTask(Event {1, 7});
		queue.process();
		REQUIRE(task.future.get().results == std::vector<int> {7});
	}
	SECTION("Void report and move") {
		eventpp::EventQueue<int, void()> queue;
		int calls = 0;
		queue.appendListener(1, [&] { ++calls; });
		auto task = queue.enqueueTask(1);
		auto moved = std::move(task);
		REQUIRE_FALSE(task.cancel());
		queue.process();
		REQUIRE(moved.future.get().errors.empty());
		REQUIRE(calls == 1);
	}
	SECTION("Move-only arguments and returns") {
		eventpp::EventQueue<int, std::unique_ptr<int>(std::unique_ptr<int> &), PointerPolicies> queue;
		queue.appendListener(1, [](std::unique_ptr<int> & value) { return std::move(value); });
		auto task = queue.enqueueTask(1, std::make_unique<int>(9));
		queue.process();
		REQUIRE(*task.future.get().results[0] == 9);
	}
	SECTION("Single threading") {
		eventpp::EventQueue<int, int(int), SinglePolicies> queue;
		queue.appendListener(1, [](int n) { return n; });
		queue.setResultContinuation(1, [](const decltype(queue)::Handle &, const int &, const int &) { return false; });
		auto task = queue.enqueueTask(1, 9);
		queue.process();
		REQUIRE(task.future.get().stoppedByResult);
		REQUIRE(queue.enqueueTask(1, 9).cancel());
	}
	SECTION("Ordered queue list") {
		eventpp::EventQueue<int, int(int), OrderedPolicies> queue;
		queue.appendListener(1, [](int n) { return n; });
		auto task = queue.enqueueTask(1, 9);
		REQUIRE(task.cancel());
		queue.processIf([](int) { return false; });
		REQUIRE(queue.emptyQueue());
	}
}

TEST_CASE("Queue task futures report errors and use current result continuation")
{
	Queue queue;
	queue.appendListener(1, [](int) -> int { throw std::runtime_error("handler"); });
	auto failure = queue.enqueueTask(1, 1);
	queue.process();
	REQUIRE_THROWS_WITH(failure.future.get(), "handler");
	REQUIRE_FALSE(failure.cancel());
	queue.appendListener(1, [](int n) { return n; });
	queue.appendListener(1, [](int n) { return n + 1; });
	auto task = queue.enqueueTask(1, 7);
	queue.setListenerExceptionPolicy(1, Queue::ExceptionPolicy::Continue);
	queue.setResultContinuation(1, [](const Queue::Handle &, const int &, const int &) { return false; });
	queue.process();
	auto report = task.future.get();
	REQUIRE(report.errors.size() == 1);
	REQUIRE(report.results == std::vector<int> {7});
	REQUIRE(report.stoppedByResult);
}

TEST_CASE("Queue tasks cancel unstarted batch work but never interrupt running work", "[thread]")
{
	Queue queue;
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool release = false;
	int calls = 0;
	queue.appendListener(1, [&](int n) {
		++calls;
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		condition.notify_one();
		condition.wait(lock, [&] { return release; });
		return n;
	});
	auto running = queue.enqueueTask(1, 1);
	auto pending = queue.enqueueTask(1, 2);
	std::thread worker([&] { queue.process(); });
	{
		std::unique_lock<std::mutex> lock(mutex);
		condition.wait(lock, [&] { return entered; });
	}
	const bool runningCancelled = running.cancel();
	const bool pendingCancelled = pending.cancel();
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
		condition.notify_one();
	}
	worker.join();
	REQUIRE_FALSE(runningCancelled);
	REQUIRE(pendingCancelled);
	REQUIRE(running.future.get().results == std::vector<int> {1});
	REQUIRE_THROWS_AS(pending.future.get(), eventpp::QueuedEventCancelled);
	REQUIRE(calls == 1);
}

TEST_CASE("Queue task concurrent cancellation and execution have one winner", "[thread]")
{
	constexpr int count = 256;
	Queue queue;
	std::vector<std::atomic<int>> calls(count);
	for(auto & value : calls) { value = 0; }
	queue.appendListener(1, [&](int id) { ++calls[id]; return id; });
	std::vector<Queue::QueuedTask> tasks;
	std::vector<int> cancelled(count);
	for(int id = 0; id < count; ++id) { tasks.push_back(queue.enqueueTask(1, id)); }
	test_threading::Barrier start(3);
	std::thread canceller([&] {
		start.arriveAndWait();
		for(int id = 0; id < count; ++id) { cancelled[id] = tasks[id].cancel(); std::this_thread::yield(); }
	});
	std::thread consumer([&] {
		start.arriveAndWait();
		while(queue.processOne()) { std::this_thread::yield(); }
	});
	start.arriveAndWait();
	canceller.join();
	consumer.join();
	for(int id = 0; id < count; ++id) {
		if(cancelled[id]) {
			REQUIRE_THROWS_AS(tasks[id].future.get(), eventpp::QueuedEventCancelled);
			REQUIRE(calls[id].load() == 0);
		}
		else {
			REQUIRE(tasks[id].future.get().results == std::vector<int> {id});
			REQUIRE(calls[id].load() == 1);
		}
	}
	REQUIRE(queue.emptyQueue());
}

TEST_CASE("Queue tasks, cancellation, clear and saved dispatch share one completion", "[thread]")
{
	for(int iteration = 0; iteration < 32; ++iteration) {
		Queue queue;
		std::atomic<int> calls {0};
		queue.appendListener(1, [&](int n) { ++calls; return n; });
		auto task = queue.enqueueTask(1, 11);
		Queue::QueuedEvent saved;
		REQUIRE(queue.peekEvent(&saved));
		test_threading::Barrier start(4);
		std::thread canceller([&] { start.arriveAndWait(); task.cancel(); });
		std::thread clearer([&] { start.arriveAndWait(); queue.clearEvents(); });
		std::thread dispatcher([&] { start.arriveAndWait(); queue.dispatch(saved); });
		start.arriveAndWait();
		canceller.join();
		clearer.join();
		dispatcher.join();
		REQUIRE(task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
		try {
			auto report = task.future.get();
			REQUIRE(report.results == std::vector<int> {11});
			REQUIRE(calls.load() == 1);
		}
		catch(const eventpp::QueuedEventCancelled &) { REQUIRE(calls.load() == 0); }
	}
}
