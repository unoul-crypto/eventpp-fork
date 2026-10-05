#include "test.h"
#include "test_threading.h"
#include "eventpp/eventqueue.h"
#include "eventpp/hetereventqueue.h"

#include <atomic>
#include <thread>

TEST_CASE("CallbackList concurrent traversal, removal and empty queries", "[thread][race-regression]")
{
	eventpp::CallbackList<void()> callbacks;
	eventpp::CallbackList<void()> queried;
	std::atomic<int> calls {0};
	const auto permanent = callbacks.append([&] { ++calls; std::this_thread::yield(); });
	test_threading::Barrier start(3);
	std::thread writer([&] {
		start.arriveAndWait();
		for(int i = 0; i < 4096; ++i) {
			auto handle = callbacks.append([&] { ++calls; });
			std::this_thread::yield();
			callbacks.remove(handle);
			auto queriedHandle = queried.append([] {});
			queried.remove(queriedHandle);
		}
	});
	std::thread reader([&] {
		start.arriveAndWait();
		for(int i = 0; i < 4096; ++i) {
			callbacks();
			callbacks.forEach([](const auto &) {});
		}
	});
	start.arriveAndWait();
	for(int i = 0; i < 4096; ++i) { (void)queried.empty(); std::this_thread::yield(); }
	writer.join();
	reader.join();
	callbacks.remove(permanent);
	REQUIRE(callbacks.empty());
	REQUIRE(calls.load() > 0);
}

TEST_CASE("Dispatcher concurrent removal during ordinary and report dispatch", "[thread][race-regression]")
{
	using Dispatcher = eventpp::EventDispatcher<int, int(int)>;
	Dispatcher dispatcher;
	dispatcher.appendListener(1, [](int value) { std::this_thread::yield(); return value; });
	test_threading::Barrier start(3);
	std::atomic<bool> valid {true};
	std::thread writer([&] {
		start.arriveAndWait();
		for(int i = 0; i < 4096; ++i) {
			auto handle = dispatcher.appendListener(1, [](int value) { return value; });
			dispatcher.setListenerExceptionPolicy(1, i % 2
				? eventpp::ListenerExceptionPolicy::Continue : eventpp::ListenerExceptionPolicy::Propagate);
			std::this_thread::yield();
			dispatcher.removeListener(1, handle);
		}
	});
	std::thread reader([&] {
		start.arriveAndWait();
		for(int i = 0; i < 4096; ++i) {
			auto report = dispatcher.dispatchWithReport(1, i);
			if(report.results.empty() || report.results.size() != report.handles.size()) { valid = false; }
			for(auto value : report.results) { if(value != i) { valid = false; } }
		}
	});
	start.arriveAndWait();
	for(int i = 0; i < 4096; ++i) { dispatcher.dispatch(1, i); }
	writer.join();
	reader.join();
	REQUIRE(valid.load());
	REQUIRE(dispatcher.getListeners(1).size() == 1);
}

TEST_CASE("Queue concurrent producers, consumers, inspection and cancellation", "[thread][race-regression]")
{
	using Queue = eventpp::EventQueue<int, int(int)>;
	constexpr int count = 2048;
	Queue queue;
	std::vector<std::atomic<int>> calls(count);
	for(auto & value : calls) { value = 0; }
	queue.appendListener(1, [&](int id) { ++calls[id]; return id; });
	std::vector<Queue::ResultFuture> futures(count);
	std::atomic<int> producers {2};
	test_threading::Barrier start(6);
	std::vector<std::thread> workers;
	for(int producer = 0; producer < 2; ++producer) {
		workers.emplace_back([&, producer] {
			start.arriveAndWait();
			for(int id = producer; id < count; id += 2) {
				futures[id] = queue.enqueueWithResults(1, id);
				std::this_thread::yield();
			}
			--producers;
		});
	}
	for(int consumer = 0; consumer < 3; ++consumer) {
		workers.emplace_back([&, consumer] {
			start.arriveAndWait();
			int iteration = consumer;
			do {
				switch(iteration++ % 7) {
				case 0: queue.process(); break;
				case 1: queue.processOne(); break;
				case 2: queue.processIf([](int id) { return id % 2 == 0; }); break;
				case 3: queue.processUntil([](int id) { return id % 3 == 0; }); break;
				case 4: {
					Queue::QueuedEvent item;
					if(queue.takeEvent(&item)) { queue.dispatch(item); }
					break;
				}
				case 5: {
					Queue::QueuedEvent item;
					if(queue.peekEvent(&item)) { queue.dispatch(item); }
					queue.clearEvents();
					break;
				}
				case 6: (void)queue.waitFor(std::chrono::milliseconds(0)); break;
				}
				std::this_thread::yield();
			} while(producers.load() != 0 || !queue.emptyQueue());
		});
	}
	start.arriveAndWait();
	for(auto & worker : workers) { worker.join(); }
	REQUIRE(queue.emptyQueue());
	for(int id = 0; id < count; ++id) {
		REQUIRE(futures[id].wait_for(std::chrono::seconds(0)) == std::future_status::ready);
		try {
			REQUIRE(futures[id].get().results == std::vector<int> {id});
			REQUIRE(calls[id].load() == 1);
		}
		catch(const std::future_error & error) {
			REQUIRE(error.code() == std::make_error_code(std::future_errc::broken_promise));
			REQUIRE(calls[id].load() == 0);
		}
	}
}

TEST_CASE("Queue empty query includes in-flight handlers and supports reentry", "[thread][race-regression]")
{
	eventpp::EventQueue<int, void()> queue;
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool release = false;
	bool sawInFlight = false;
	int nestedCalls = 0;
	queue.appendListener(2, [&] { ++nestedCalls; });
	queue.appendListener(1, [&] {
		sawInFlight = !queue.emptyQueue();
		queue.enqueue(2);
		queue.wait();
		queue.processOne();
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		condition.notify_one();
		condition.wait(lock, [&] { return release; });
	});
	queue.enqueue(1);
	std::thread worker([&] { queue.process(); });
	bool during;
	{
		std::unique_lock<std::mutex> lock(mutex);
		condition.wait(lock, [&] { return entered; });
		during = queue.emptyQueue();
		release = true;
		condition.notify_one();
	}
	worker.join();
	REQUIRE_FALSE(during);
	REQUIRE(sawInFlight);
	REQUIRE(nestedCalls == 1);
	REQUIRE(queue.emptyQueue());
	REQUIRE_FALSE(queue.waitFor(std::chrono::milliseconds(1)));
}

TEST_CASE("Heterogeneous queue concurrent insertion, processing and inspection", "[thread][race-regression]")
{
	eventpp::HeterEventQueue<int, eventpp::HeterTuple<void(int)>> queue;
	std::atomic<int> calls {0};
	queue.appendListener(1, [&](int) { ++calls; });
	test_threading::Barrier start(4);
	std::atomic<bool> done {false};
	std::thread producer([&] {
		start.arriveAndWait();
		for(int i = 0; i < 4096; ++i) { queue.enqueue(1, i); std::this_thread::yield(); }
		done = true;
	});
	std::thread consumer([&] {
		start.arriveAndWait();
		while(!done.load() || !queue.emptyQueue()) {
			queue.process();
			queue.processOne();
			queue.processIf([](int) { return true; });
		}
	});
	std::thread observer([&] {
		start.arriveAndWait();
		while(!done.load()) {
			(void)queue.emptyQueue();
			(void)queue.waitFor(std::chrono::milliseconds(0));
			queue.clearEvents();
		}
	});
	start.arriveAndWait();
	producer.join();
	consumer.join();
	observer.join();
	REQUIRE(queue.emptyQueue());
	REQUIRE(calls.load() <= 4096);
}
