// This target deliberately uses C++11, independently of the unit test standard.
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"

#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>

struct Policies {
	using ListenerMetadata = std::map<std::string, int>;
	using AggregationResult = long long;
};

int main()
{
	using Dispatcher = eventpp::EventDispatcher<int, int(int), Policies>;
	Dispatcher dispatcher;
	auto good = dispatcher.appendListener(1, [](int n) { return n + 1; }, {{"factor", 2}});
	auto bad = dispatcher.appendListener(1, [](int) -> int { throw std::runtime_error("failed"); });
	if(!dispatcher.updateListenerMetadata(1, good, [](Dispatcher::ListenerMetadata & metadata) {
		++metadata["factor"];
	})) { return 1; }
	Dispatcher::ListenerMetadata metadata;
	if(!dispatcher.getListenerMetadata(1, good, metadata) || metadata.at("factor") != 3) { return 2; }
	if(dispatcher.getListeners(1).size() != 2) { return 3; }
	dispatcher.setListenerPlanner(1, [=](const Dispatcher::ListenerList & listeners, const int & n) {
		Dispatcher::ListenerPlan plan;
		plan.add(listeners[0].handle, n * listeners[0].metadata.at("factor"));
		plan.add(bad);
		return plan;
	});
	dispatcher.setResultAggregator(1, [](const Dispatcher::ListenerResults & values) {
		return std::accumulate(values.begin(), values.end(), 0LL);
	});
	dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Continue);
	auto report = dispatcher.dispatchWithReport(1, 3);
	if(report.results != std::vector<int>{10} || report.handles.size() != 1
		|| report.handles[0].lock() != good.lock() || report.errors.size() != 1
		|| report.errors[0].handle.lock() != bad.lock() || !report.aggregate || *report.aggregate != 10) { return 4; }
	try { std::rethrow_exception(report.errors[0].exception); return 5; }
	catch(const std::runtime_error &) {}
	dispatcher.clearListenerPlanner(1);
	dispatcher.setListenerOrdering(1, [](const Dispatcher::ListenerList & listeners, const int &) {
		return Dispatcher::ListenerOrder{listeners[0].handle};
	});
	if(dispatcher.dispatchWithResults(1, 3).results != std::vector<int>{4}) { return 6; }
	dispatcher.clearListenerOrdering(1);
	dispatcher.setListenerExceptionPolicy(1, Dispatcher::ExceptionPolicy::Propagate);
	try { dispatcher.dispatchWithReport(1, 3); return 7; }
	catch(const std::runtime_error &) {}

	eventpp::EventQueue<int, int(int)> queue;
	queue.appendListener(1, [](int n) { return n * 2; });
	auto future = queue.enqueueWithResults(1, 7);
	queue.process();
	if(future.get().results != std::vector<int>{14}) { return 8; }

	eventpp::EventQueue<int, void()> notifications;
	int completed = 0;
	notifications.appendListener(1, [] { throw std::runtime_error("notification"); });
	notifications.appendListener(1, [&completed] { ++completed; });
	notifications.setListenerExceptionPolicy(1, eventpp::ListenerExceptionPolicy::Continue);
	auto notification = notifications.enqueueWithReport(1);
	notifications.process();
	if(notification.get().errors.size() != 1 || completed != 1) { return 9; }

	eventpp::EventDispatcher<int, std::unique_ptr<int>()> owned;
	owned.appendListener(1, [] { return std::unique_ptr<int>(new int(5)); });
	auto ownedReport = owned.dispatchWithReport(1);
	if(ownedReport.results.size() != 1 || *ownedReport.results[0] != 5) { return 10; }
	return 0;
}
