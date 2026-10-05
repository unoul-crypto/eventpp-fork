// eventpp library
// Copyright (C) 2018 Wang Qi (wqking)
// Github: https://github.com/wqking/eventpp
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//   http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef EVENTQUEUE_H_705786053037
#define EVENTQUEUE_H_705786053037

#include "eventdispatcher.h"
#include "internal/eventqueue_i.h"

#include <tuple>
#include <chrono>
#include <future>

namespace eventpp {

namespace internal_ {

template <
	typename EventType_,
	typename Prototype_,
	typename Policies_
>
class EventQueueBase;

template <
	typename EventType_,
	typename Policies_,
	typename ReturnType, typename ...Args
>
class EventQueueBase <
		EventType_,
		ReturnType (Args...),
		Policies_
	> : public EventDispatcherBase<
		EventType_,
		ReturnType (Args...),
		Policies_,
		EventQueueBase <
			EventType_,
			ReturnType (Args...),
			Policies_
		>
	>
{
private:
	using super = EventDispatcherBase<
		EventType_,
		ReturnType (Args...),
		Policies_,
		EventQueueBase <
			EventType_,
			ReturnType (Args...),
			Policies_
		>
	>;

	using Policies = typename super::Policies;
	using Threading = typename super::Threading;
	using ConditionVariable = typename Threading::ConditionVariable;

	using QueuedEventArgumentsType = std::tuple<typename std::decay<Args>::type...>;

	struct QueuedResultState
	{
		QueuedResultState() : promise(new std::promise<typename super::DispatchResult>()), started(false) {}

		void cancel() noexcept
		{
			if(! started.exchange(true)) { promise.reset(); }
		}

		std::unique_ptr<std::promise<typename super::DispatchResult> > promise;
		typename Threading::template Atomic<bool> started;
	};

	struct QueuedEvent_
	{
		typename std::decay<typename super::Event>::type event;
		QueuedEventArgumentsType arguments;
		std::shared_ptr<QueuedResultState> resultState;

		typename super::Event getEvent() const {
			return event;
		}

		template <std::size_t N>
		auto getArgument() const
			-> typename std::tuple_element<N, std::tuple<Args...> >::type {
			return std::get<N>(arguments);
		}
	};

	using BufferedItemList = typename SelectQueueList<
		BufferedItem<QueuedEvent_>, 
		Policies_,
		HasTemplateQueueList<Policies_>::value
	>::Type;

public:
	using QueuedEvent = QueuedEvent_;
	using Event = typename super::Event;
	using Handle = typename super::Handle;
	using Callback = typename super::Callback;
	using Mutex = typename super::Mutex;
	using DispatchResult = typename super::DispatchResult;
	using ResultFuture = std::future<DispatchResult>;

	struct DisableQueueNotify
	{
		DisableQueueNotify(EventQueueBase * queue)
			: queue(queue)
		{
			++queue->queueNotifyCounter;
		}

		~DisableQueueNotify()
		{
			--queue->queueNotifyCounter;

			if(queue->doCanNotifyQueueAvailable() && ! queue->emptyQueue()) {
				queue->queueListConditionVariable.notify_one();
			}
		}

		EventQueueBase * queue;
	};

public:
	EventQueueBase()
		:
			super(),
			queueListConditionVariable(),
			queueEmptyCounter(0),
			queueNotifyCounter(0),
			queueListMutex(),
			queueList(),
			freeListMutex(),
			freeList()
	{
	}

	EventQueueBase(const EventQueueBase & other)
		: super(other)
	{
	}

	EventQueueBase(EventQueueBase && other) noexcept
		: super(std::move(other))
	{
	}

	EventQueueBase & operator = (const EventQueueBase & other)
	{
		super::operator = (other);
		return *this;
	}
	
	EventQueueBase & operator = (EventQueueBase && other) noexcept
	{
		super::operator = (std::move(other));
		return *this;
	}

	~EventQueueBase()
	{
		// Destruction is not concurrent with other queue operations.
		for(auto & item : queueList) {
			if(! item.empty() && item.get().resultState) { item.get().resultState->cancel(); }
		}
	}

	template <typename ...A>
	auto enqueueWithResults(A && ...args)
		-> typename std::enable_if<sizeof...(A) == sizeof...(Args) && CanCollectReturn<ReturnType>::value, ResultFuture>::type
	{
		return enqueueWithReport(std::forward<A>(args)...);
	}

	template <typename T, typename ...A>
	auto enqueueWithResults(T && first, A && ...args)
		-> typename std::enable_if<sizeof...(A) == sizeof...(Args) && CanCollectReturn<ReturnType>::value, ResultFuture>::type
	{
		return enqueueWithReport(std::forward<T>(first), std::forward<A>(args)...);
	}

	template <typename ...A>
	auto enqueueWithReport(A && ...args)
		-> typename std::enable_if<sizeof...(A) == sizeof...(Args), ResultFuture>::type
	{
		static_assert(super::ArgumentPassingMode::canIncludeEventType, "Event type should be included in enqueue arguments.");
		using GetEvent = typename SelectGetEvent<Policies_, EventType_, HasFunctionGetEvent<Policies_, A...>::value>::Type;
		auto state = std::make_shared<QueuedResultState>();
		auto future = state->promise->get_future();
		doEnqueue(QueuedEvent { GetEvent::getEvent(args...),
			QueuedEventArgumentsType(std::forward<A>(args)...), std::move(state) });
		if(doCanNotifyQueueAvailable()) { queueListConditionVariable.notify_one(); }
		return future;
	}

	template <typename T, typename ...A>
	auto enqueueWithReport(T && first, A && ...args)
		-> typename std::enable_if<sizeof...(A) == sizeof...(Args), ResultFuture>::type
	{
		static_assert(super::ArgumentPassingMode::canExcludeEventType, "Event type should not be included in callback arguments.");
		using GetEvent = typename SelectGetEvent<Policies_, EventType_, HasFunctionGetEvent<Policies_, T &&, A...>::value>::Type;
		auto state = std::make_shared<QueuedResultState>();
		auto future = state->promise->get_future();
		doEnqueue(QueuedEvent { GetEvent::getEvent(std::forward<T>(first), args...),
			QueuedEventArgumentsType(std::forward<A>(args)...), std::move(state) });
		if(doCanNotifyQueueAvailable()) { queueListConditionVariable.notify_one(); }
		return future;
	}

	template <typename ...A>
	auto enqueue(A && ...args) -> typename std::enable_if<sizeof...(A) == sizeof...(Args), void>::type
	{
		static_assert(super::ArgumentPassingMode::canIncludeEventType, "Enqueuing arguments count doesn't match required (Event type should be included).");

		using GetEvent = typename SelectGetEvent<Policies_, EventType_, HasFunctionGetEvent<Policies_, A...>::value>::Type;

		doEnqueue(QueuedEvent{
			GetEvent::getEvent(args...),
			QueuedEventArgumentsType(std::forward<A>(args)...), nullptr
		});

		if(doCanNotifyQueueAvailable()) {
			queueListConditionVariable.notify_one();
		}
	}

	template <typename T, typename ...A>
	auto enqueue(T && first, A && ...args) -> typename std::enable_if<sizeof...(A) == sizeof...(Args), void>::type
	{
		static_assert(super::ArgumentPassingMode::canExcludeEventType, "Enqueuing arguments count doesn't match required (Event type should NOT be included).");

		using GetEvent = typename SelectGetEvent<Policies_, EventType_, HasFunctionGetEvent<Policies_, T &&, A...>::value>::Type;

		doEnqueue(QueuedEvent{
			GetEvent::getEvent(std::forward<T>(first), args...),
			QueuedEventArgumentsType(std::forward<A>(args)...), nullptr
		});

		if(doCanNotifyQueueAvailable()) {
			queueListConditionVariable.notify_one();
		}
	}

	bool emptyQueue() const
	{
		std::lock_guard<Mutex> queueListLock(queueListMutex);
		return doEmptyQueueLocked();
	}
	
	void clearEvents()
	{
		std::unique_lock<Mutex> queueListAccess(queueListMutex);
		if(! queueList.empty()) {
			BufferedItemList tempList;

			std::swap(queueList, tempList);
			queueListAccess.unlock();

			if(! tempList.empty()) {
				for(auto & item : tempList) {
					if(item.get().resultState) { item.get().resultState->cancel(); }
					item.clear();
				}

				std::lock_guard<Mutex> queueListLock(freeListMutex);
				freeList.splice(freeList.end(), tempList);
			}
		}
	}

	bool process()
	{
		std::unique_lock<Mutex> queueListAccess(queueListMutex);
		if(! queueList.empty()) {
			BufferedItemList tempList;

			// Use a counter to tell the queue list is not empty during processing
			// even though queueList is swapped to empty.
			CounterGuard<decltype(queueEmptyCounter)> counterGuard(queueEmptyCounter);

			std::swap(queueList, tempList);
			queueListAccess.unlock();

			if(! tempList.empty()) {
				for(auto & item : tempList) {
					doDispatchQueuedEvent(
						item.get(),
						typename MakeIndexSequence<sizeof...(Args)>::Type(), &tempList
					);
					item.clear();
				}

				std::lock_guard<Mutex> queueListLock(freeListMutex);
				freeList.splice(freeList.end(), tempList);
				
				return true;
			}
		}
		
		return false;
	}

	bool processOne()
	{
		std::unique_lock<Mutex> queueListAccess(queueListMutex);
		if(! queueList.empty()) {
			BufferedItemList tempList;

			// Use a counter to tell the queue list is not empty during processing
			// even though queueList is swapped to empty.
			CounterGuard<decltype(queueEmptyCounter)> counterGuard(queueEmptyCounter);

			tempList.splice(tempList.end(), queueList, queueList.begin());
			queueListAccess.unlock();

			if(! tempList.empty()) {
				auto & item = tempList.front();
				doDispatchQueuedEvent(
				item.get(),
				typename MakeIndexSequence<sizeof...(Args)>::Type(), &tempList
				);
				item.clear();

				std::lock_guard<Mutex> queueListLock(freeListMutex);
				freeList.splice(freeList.end(), tempList);
				
				return true;
			}
		}
		
		return false;
	}

	template <typename Predictor>
	bool processIf(Predictor && predictor)
	{
		std::unique_lock<Mutex> queueListAccess(queueListMutex);
		if(! queueList.empty()) {
			BufferedItemList tempList;
			BufferedItemList idleList;

			// Use a counter to tell the queue list is not empty during processing
			// even though queueList is swapped to empty.
			CounterGuard<decltype(queueEmptyCounter)> counterGuard(queueEmptyCounter);

			std::swap(queueList, tempList);
			queueListAccess.unlock();

			if(! tempList.empty()) {
				for(auto it = tempList.begin(); it != tempList.end(); ) {
					if(doInvokeFuncWithQueuedEvent(
							predictor,
							it->get(),
							typename MakeIndexSequence<sizeof...(Args)>::Type(), &tempList)
						) {
						doDispatchQueuedEvent(
							it->get(),
							typename MakeIndexSequence<sizeof...(Args)>::Type(), &tempList
						);
						it->clear();
						
						auto tempIt = it;
						++it;
						idleList.splice(idleList.end(), tempList, tempIt);
					}
					else {
						++it;
					}
				}

				if (! tempList.empty()) {
					std::lock_guard<Mutex> queueListLock(queueListMutex);
					queueList.splice(queueList.begin(), tempList);
				}

				if(! idleList.empty()) {
					std::lock_guard<Mutex> queueListLock(freeListMutex);
					freeList.splice(freeList.end(), idleList);
					
					return true;
				}
			}
		}
		
		return false;
	}
	
	template <typename Predictor>
	bool processUntil(Predictor && predictor)
	{
		std::unique_lock<Mutex> queueListAccess(queueListMutex);
		if(! queueList.empty()) {
			BufferedItemList tempList;
			BufferedItemList idleList;

			// Use a counter to tell the queue list is not empty during processing
			// even though queueList is swapped to empty.
			CounterGuard<decltype(queueEmptyCounter)> counterGuard(queueEmptyCounter);

			std::swap(queueList, tempList);
			queueListAccess.unlock();

			if(! tempList.empty()) {
				for(auto it = tempList.begin(); it != tempList.end(); ) {
					if(doInvokeFuncWithQueuedEvent(
							predictor,
							it->get(),
							typename MakeIndexSequence<sizeof...(Args)>::Type(), &tempList)
						) {
						break;
					}
					else {
						doDispatchQueuedEvent(
							it->get(),
							typename MakeIndexSequence<sizeof...(Args)>::Type(), &tempList
						);
						it->clear();
						
						auto tempIt = it;
						++it;
						idleList.splice(idleList.end(), tempList, tempIt);
					}
				}

				if (! tempList.empty()) {
					std::lock_guard<Mutex> queueListLock(queueListMutex);
					queueList.splice(queueList.begin(), tempList);
				}

				if(! idleList.empty()) {
					std::lock_guard<Mutex> queueListLock(freeListMutex);
					freeList.splice(freeList.end(), idleList);
					
					return true;
				}
			}
		}
		
		return false;
	}
	
	void wait() const
	{
		std::unique_lock<Mutex> queueListLock(queueListMutex);
		queueListConditionVariable.wait(queueListLock, [this]() -> bool {
			return ! doEmptyQueueLocked() && doCanNotifyQueueAvailable();
		});
	}

	template <class Rep, class Period>
	bool waitFor(const std::chrono::duration<Rep, Period> & duration) const
	{
		std::unique_lock<Mutex> queueListLock(queueListMutex);
		return queueListConditionVariable.wait_for(queueListLock, duration, [this]() -> bool {
			return ! doEmptyQueueLocked() && doCanNotifyQueueAvailable();
		});
	}

	using super::dispatch;

	template <typename U>
	auto dispatch(U & queuedEvent)
		-> typename std::enable_if<std::is_same<U, QueuedEvent>::value, void>::type
	{
		doDispatchQueuedEvent(queuedEvent, typename MakeIndexSequence<sizeof...(Args)>::Type());
	}

	template <typename U>
	auto dispatch(const U & queuedEvent)
		-> typename std::enable_if<std::is_same<U, QueuedEvent>::value, void>::type
	{
		doDispatchQueuedEvent(
			queuedEvent,
			typename MakeIndexSequence<sizeof...(Args)>::Type()
		);
	}

	bool peekEvent(QueuedEvent * queuedEvent)
	{
		std::lock_guard<Mutex> queueListLock(queueListMutex);
		if(! queueList.empty()) {
			*queuedEvent = queueList.front().get();
			return true;
		}
		return false;
	}

	bool takeEvent(QueuedEvent * queuedEvent)
	{
		std::unique_lock<Mutex> queueListAccess(queueListMutex);
		if(! queueList.empty()) {
			BufferedItemList tempList;

			tempList.splice(tempList.end(), queueList, queueList.begin());
			queueListAccess.unlock();

			if(! tempList.empty()) {
				*queuedEvent = std::move(tempList.front().get());
				tempList.front().clear();

				std::lock_guard<Mutex> queueListLock(freeListMutex);
				freeList.splice(freeList.end(), tempList);

				return true;
			}
		}

		return false;
	}

protected:
	bool doCanProcess() const
	{
		return ! emptyQueue() && doCanNotifyQueueAvailable();
	}

	bool doCanNotifyQueueAvailable() const
	{
		return queueNotifyCounter.load(std::memory_order_acquire) == 0;
	}

	template <typename T, size_t ...Indexes>
	void doDispatchQueuedEvent(T && item, IndexSequence<Indexes...>, const BufferedItemList * batch = nullptr)
	{
		try {
			if(item.resultState) {
				doDispatchQueuedResult(item, IndexSequence<Indexes...>());
			}
			else {
				this->directDispatch(item.event, std::get<Indexes>(item.arguments)...);
			}
		}
		catch(...) {
			// Ordinary enqueue exceptions still propagate. Abandoned batch futures
			// must complete even when a peek copy keeps their shared state alive.
			doCancelBatch(batch);
			throw;
		}
	}

	template <typename T, size_t ...Indexes>
	void doDispatchQueuedResult(T & item, IndexSequence<Indexes...>)
	{
		auto state = item.resultState;
		if(state->started.exchange(true)) { return; }
		auto promise = std::move(state->promise);
		try {
			promise->set_value(this->directDispatchWithReport(item.event, std::get<Indexes>(item.arguments)...));
		}
		catch(...) {
			promise->set_exception(std::current_exception());
		}
	}

	template <typename F, typename T, size_t ...Indexes>
	bool doInvokeFuncWithQueuedEvent(F && func, T && item, IndexSequence<Indexes...>,
		const BufferedItemList * batch = nullptr) const
	{
		try {
			return doInvokeFuncWithQueuedEventHelper(std::forward<F>(func), std::get<Indexes>(item.arguments)...);
		}
		catch(...) {
			doCancelBatch(batch);
			throw;
		}
	}

	static void doCancelBatch(const BufferedItemList * batch) noexcept
	{
		if(batch) {
			for(const auto & pending : *batch) {
				if(! pending.empty() && pending.get().resultState) { pending.get().resultState->cancel(); }
			}
		}
	}
	
	template <typename F>
	auto doInvokeFuncWithQueuedEventHelper(F && func, Args ...args) const
		-> typename std::enable_if<! CanInvoke<F>::value, bool>::type
	{
		return func(std::forward<Args>(args)...);
	}

	template <typename F>
	auto doInvokeFuncWithQueuedEventHelper(F && func, Args .../*args*/) const
		-> typename std::enable_if<CanInvoke<F>::value, bool>::type
	{
		return func();
	}

	void doEnqueue(QueuedEvent && item)
	{
		BufferedItemList tempList;
		{
			std::lock_guard<Mutex> freeListLock(freeListMutex);
			if(! freeList.empty()) {
				tempList.splice(tempList.end(), freeList, freeList.begin());
			}
		}

		if(tempList.empty()) {
			tempList.emplace_back();
		}

		auto it = tempList.begin();
		it->set(std::move(item));

		std::lock_guard<Mutex> queueListLock(queueListMutex);
		queueList.splice(queueList.end(), tempList, it);
	}

private:
	// Caller holds queueListMutex; wait predicates must not lock it again.
	bool doEmptyQueueLocked() const
	{
		return queueList.empty() && (queueEmptyCounter.load(std::memory_order_acquire) == 0);
	}

	mutable ConditionVariable queueListConditionVariable;
	typename Threading::template Atomic<int> queueEmptyCounter {0};
	typename Threading::template Atomic<int> queueNotifyCounter {0};
	mutable Mutex queueListMutex;
	BufferedItemList queueList;
	Mutex freeListMutex;
	BufferedItemList freeList;
};

} //namespace internal_

template <
	typename Event_,
	typename Prototype_,
	typename Policies_ = DefaultPolicies
>
class EventQueue : public internal_::InheritMixins<
		internal_::EventQueueBase<Event_, Prototype_, Policies_>,
		typename internal_::SelectMixins<Policies_, internal_::HasTypeMixins<Policies_>::value >::Type
	>::Type, public TagEventDispatcher, public TagEventQueue
{
private:
	using super = typename internal_::InheritMixins<
		internal_::EventQueueBase<Event_, Prototype_, Policies_>,
		typename internal_::SelectMixins<Policies_, internal_::HasTypeMixins<Policies_>::value >::Type
	>::Type;

public:
	using super::super;
};


} //namespace eventpp


#endif
