#ifndef EVENTPP_TEST_THREADING_H
#define EVENTPP_TEST_THREADING_H

#include <condition_variable>
#include <mutex>

namespace test_threading {
// Sanitizers multiply memory and runtime costs. Keep the same operations with
// fewer subscriptions; ordinary builds retain the original stress workload.
#ifdef EVENTPP_TEST_SANITIZED
constexpr int threadCount = 8;
constexpr int itemsPerThread = 256;
constexpr int insertItemsPerThread = 128;
#else
constexpr int threadCount = 256;
constexpr int itemsPerThread = 1024 * 4;
constexpr int insertItemsPerThread = 1024;
#endif

class Barrier {
public:
	explicit Barrier(int count) : remaining(count) {}
	void arriveAndWait() {
		std::unique_lock<std::mutex> lock(mutex);
		if(--remaining == 0) { condition.notify_all(); }
		else { condition.wait(lock, [this] { return remaining == 0; }); }
	}
private:
	std::mutex mutex;
	std::condition_variable condition;
	int remaining;
};
}

#endif
