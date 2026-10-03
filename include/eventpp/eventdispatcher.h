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

#ifndef EVENTDISPATCHER_H_319010983013
#define EVENTDISPATCHER_H_319010983013

#include "callbacklist.h"

#include <memory>
#include <tuple>

namespace eventpp {

namespace internal_ {

template <
	typename EventType_,
	typename Prototype_,
	typename Policies_,
	typename MixinRoot_
>
class EventDispatcherBase;

template <
	typename EventType_,
	typename Policies_,
	typename MixinRoot_,
	typename ReturnType, typename ...Args
>
class EventDispatcherBase <
	EventType_,
	ReturnType (Args...),
	Policies_,
	MixinRoot_
>
{
protected:
	using ThisType = EventDispatcherBase<
		EventType_,
		ReturnType (Args...),
		Policies_,
		MixinRoot_
	>;
	using MixinRoot = typename std::conditional<
		std::is_same<MixinRoot_, void>::value,
		ThisType,
		MixinRoot_
	>::type;
	using Policies = Policies_;

	using Threading = typename SelectThreading<Policies_, HasTypeThreading<Policies_>::value>::Type;

	using ArgumentPassingMode = typename SelectArgumentPassingMode<
		Policies_,
		HasTypeArgumentPassingMode<Policies_>::value,
		ArgumentPassingAutoDetect
	>::Type;

	using Callback_ = typename SelectCallback<
		Policies_,
		HasTypeCallback<Policies_>::value,
		std::function<ReturnType (Args...)>
	>::Type;
	using CallbackList_ = CallbackList<ReturnType (Args...), Policies_>;

	using Prototype = ReturnType (Args...);

	using Map = typename SelectMap<
		EventType_,
		CallbackList_,
		Policies_,
		HasTemplateMap<Policies_>::value
	>::Type;

	using Mixins = typename internal_::SelectMixins<
		Policies_,
		internal_::HasTypeMixins<Policies_>::value
	>::Type;

public:
	using Handle = typename CallbackList_::Handle;
	using Callback = Callback_;
	using Event = EventType_;
	using Mutex = typename Threading::Mutex;
	using ListenerMetadata = typename CallbackList_::ListenerMetadata;
	using ListenerInfo = typename CallbackList_::ListenerInfo;
	using ListenerList = typename CallbackList_::ListenerList;
	using ListenerOrder = typename CallbackList_::ListenerOrder;

	class ListenerPlan
	{
	private:
		friend ThisType;

		struct ArgumentsBase
		{
			virtual ~ArgumentsBase() = default;
			virtual bool invoke(const CallbackList_ & listeners, Callback & callback) = 0;
		};

		template <typename Tuple>
		struct Arguments : ArgumentsBase
		{
			template <typename ...Values>
			explicit Arguments(Values && ...values) : values(std::forward<Values>(values)...)
			{
			}

			bool invoke(const CallbackList_ & listeners, Callback & callback) override
			{
				return ThisType::doInvokePlannedArguments(listeners, callback, values,
					typename MakeIndexSequence<sizeof...(Args)>::Type());
			}

			Tuple values;
		};

		struct Invocation
		{
			explicit Invocation(const Handle & handle,
				std::unique_ptr<ArgumentsBase> arguments = nullptr)
				: handle(handle), arguments(std::move(arguments))
			{
			}

			Handle handle;
			std::unique_ptr<ArgumentsBase> arguments;
		};

	public:
		ListenerPlan() = default;
		ListenerPlan(ListenerPlan &&) noexcept = default;
		ListenerPlan & operator = (ListenerPlan &&) noexcept = default;
		ListenerPlan(const ListenerPlan &) = delete;
		ListenerPlan & operator = (const ListenerPlan &) = delete;

		void add(const Handle & handle)
		{
			invocations.emplace_back(handle);
		}

		template <typename ...Values>
		typename std::enable_if<(sizeof...(Values) > 0), void>::type
		add(const Handle & handle, Values && ...values)
		{
			static_assert(sizeof...(Values) == sizeof...(Args), "Replacement arguments must match the callback signature.");
			using Tuple = std::tuple<typename std::decay<Args>::type...>;
			static_assert(std::is_constructible<Tuple, Values &&...>::value,
				"Replacement arguments must be convertible to the callback argument types.");
			invocations.emplace_back(handle, std::unique_ptr<ArgumentsBase>(
				new Arguments<Tuple>(std::forward<Values>(values)...)));
		}

		std::size_t size() const noexcept { return invocations.size(); }
		bool empty() const noexcept { return invocations.empty(); }

	private:
		std::vector<Invocation> invocations;
	};

	// A selection function observes the original arguments without copying or moving them.
	template <typename Result>
	using ListenerSelectionFunction = std::function<Result(const ListenerList &,
		typename std::add_lvalue_reference<typename std::add_const<
			typename std::remove_reference<Args>::type>::type>::type...)>;
	// Observe arguments without making copies or moving values away from listeners.
	using ListenerOrdering = ListenerSelectionFunction<ListenerOrder>;
	using ListenerPlanner = ListenerSelectionFunction<ListenerPlan>;

private:
	struct ListenerSelection
	{
		ListenerOrdering ordering;
		ListenerPlanner planner;
	};
	using OrderingMap = typename SelectMap<Event, std::shared_ptr<ListenerSelection>,
		Policies_, HasTemplateMap<Policies_>::value>::Type;

public:
	EventDispatcherBase()
		:
			eventCallbackListMap(),
			listenerOrderingMap(),
			listenerMutex()
	{
	}

	EventDispatcherBase(const EventDispatcherBase & other)
		:
			eventCallbackListMap(other.eventCallbackListMap),
			listenerOrderingMap(cloneOrderingMap(other.listenerOrderingMap)),
			listenerMutex()
	{
	}

	EventDispatcherBase(EventDispatcherBase && other) noexcept
		:
			eventCallbackListMap(std::move(other.eventCallbackListMap)),
			listenerOrderingMap(std::move(other.listenerOrderingMap)),
			listenerMutex()
	{
	}

	EventDispatcherBase & operator = (const EventDispatcherBase & other)
	{
		if(this != &other) {
			EventDispatcherBase copied(other);
			swap(copied);
		}
		return *this;
	}

	EventDispatcherBase & operator = (EventDispatcherBase && other) noexcept
	{
		eventCallbackListMap = std::move(other.eventCallbackListMap);
		listenerOrderingMap = std::move(other.listenerOrderingMap);
		return *this;
	}

	void swap(EventDispatcherBase & other) noexcept {
		using std::swap;
		
		swap(eventCallbackListMap, other.eventCallbackListMap);
		swap(listenerOrderingMap, other.listenerOrderingMap);
	}

	Handle appendListener(const Event & event, const Callback & callback)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);

		return eventCallbackListMap[event].append(callback);
	}

	Handle prependListener(const Event & event, const Callback & callback)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);

		return eventCallbackListMap[event].prepend(callback);
	}

	Handle appendListener(const Event & event, const Callback & callback, const ListenerMetadata & metadata)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);
		return eventCallbackListMap[event].append(callback, metadata);
	}

	Handle prependListener(const Event & event, const Callback & callback, const ListenerMetadata & metadata)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);
		return eventCallbackListMap[event].prepend(callback, metadata);
	}

	Handle insertListener(const Event & event, const Callback & callback, const Handle & before)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);

		return eventCallbackListMap[event].insert(callback, before);
	}

	Handle insertListener(const Event & event, const Callback & callback, const Handle & before,
		const ListenerMetadata & metadata)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);
		return eventCallbackListMap[event].insert(callback, before, metadata);
	}

	void setListenerOrdering(const Event & event, const ListenerOrdering & ordering)
	{
		if(! ordering) {
			clearListenerOrdering(event);
			return;
		}
		auto stored = std::make_shared<ListenerSelection>();
		stored->ordering = ordering;
		doSetListenerSelection(event, std::move(stored));
	}

	void setListenerPlanner(const Event & event, const ListenerPlanner & planner)
	{
		if(! planner) {
			clearListenerPlanner(event);
			return;
		}
		auto stored = std::make_shared<ListenerSelection>();
		stored->planner = planner;
		doSetListenerSelection(event, std::move(stored));
	}

	void clearListenerPlanner(const Event & event)
	{
		clearListenerOrdering(event);
	}

	void clearListenerOrdering(const Event & event)
	{
		std::shared_ptr<ListenerSelection> removed;
		{
			std::lock_guard<Mutex> lockGuard(listenerMutex);
			auto it = listenerOrderingMap.find(event);
			if(it != listenerOrderingMap.end()) {
				removed = std::move(it->second);
				listenerOrderingMap.erase(it);
			}
		}
	}

	bool removeListener(const Event & event, const Handle handle)
	{
		CallbackList_ * callableList = doFindCallableList(event);
		if(callableList) {
			return callableList->remove(handle);
		}

		return false;
	}

	bool hasAnyListener(const Event & event) const
	{
		const CallbackList_ * callableList = doFindCallableList(event);
		if(callableList) {
			return ! callableList->empty();
		}

		return false;
	}

	bool ownsHandle(const Event & event, const Handle & handle) const
	{
		const CallbackList_ * callableList = doFindCallableList(event);
		if(callableList) {
			return callableList->ownsHandle(handle);
		}

		return false;
	}

	template <typename Func>
	void forEach(const Event & event, Func && func) const
	{
		const CallbackList_ * callableList = doFindCallableList(event);
		if(callableList) {
			callableList->forEach(std::forward<Func>(func));
		}
	}

	template <typename Func>
	bool forEachIf(const Event & event, Func && func) const
	{
		const CallbackList_ * callableList = doFindCallableList(event);
		if (callableList) {
			return callableList->forEachIf(std::forward<Func>(func));
		}

		return true;
	}

	void dispatch(Args ...args) const
	{
		static_assert(ArgumentPassingMode::canIncludeEventType, "Dispatching arguments count doesn't match required (Event type should be included).");

		using GetEvent = typename SelectGetEvent<Policies_, EventType_, HasFunctionGetEvent<Policies_, Args...>::value>::Type;

		// can't std::forward<Args>(args) in GetEvent::getEvent because the pass by value arguments will be moved to getEvent
		// then the other std::forward<Args>(args) to directDispatch will get empty values.
		directDispatch(
			GetEvent::getEvent(args...),
			std::forward<Args>(args)...
		);
	}

	template <typename T>
	void dispatch(T && first, Args ...args) const
	{
		static_assert(ArgumentPassingMode::canExcludeEventType, "Dispatching arguments count doesn't match required (Event type should NOT be included).");

		using GetEvent = typename SelectGetEvent<Policies_, EventType_, HasFunctionGetEvent<Policies_, T &&, Args...>::value>::Type;

		directDispatch(
			GetEvent::getEvent(std::forward<T>(first), args...),
			std::forward<Args>(args)...
		);
	}

	// Bypass any getEvent policy. The first argument is the event type.
	// Most used for internal purpose.
	void directDispatch(const Event & e, Args ...args) const
	{
		if(! internal_::ForEachMixins<MixinRoot, Mixins, DoMixinBeforeDispatch>::forEach(
			this, typename std::add_lvalue_reference<Args>::type(args)...)) {
			return;
		}

		const CallbackList_ * callableList = nullptr;
		std::shared_ptr<ListenerSelection> selection;
		{
			std::lock_guard<Mutex> lockGuard(listenerMutex);
			auto it = eventCallbackListMap.find(e);
			if(it != eventCallbackListMap.end()) {
				callableList = &it->second;
			}
			if(! listenerOrderingMap.empty()) {
				auto orderIt = listenerOrderingMap.find(e);
				if(orderIt != listenerOrderingMap.end()) {
					selection = orderIt->second;
				}
			}
		}
		if(callableList) {
			if(selection) {
				const auto listeners = callableList->doGetListeners();
				if(selection->planner) {
					auto plan = selection->planner(listeners, args...);
					callableList->doInvokeSelected(listeners, plan.invocations,
						[](const typename ListenerPlan::Invocation & call) -> const Handle & { return call.handle; },
						[&](Callback & callback, typename ListenerPlan::Invocation & call) {
							return call.arguments ? call.arguments->invoke(*callableList, callback)
								: callableList->doInvokeCallback(callback, args...);
						});
				}
				else {
					const auto order = selection->ordering(listeners, args...);
					callableList->doInvokeInOrder(listeners, order, args...);
				}
			}
			else {
				(*callableList)(std::forward<Args>(args)...);
			}
		}
	}

protected:
	const CallbackList_ * doFindCallableList(const Event & e) const
	{
		return doFindCallableListHelper(this, e);
	}

	CallbackList_ * doFindCallableList(const Event & e)
	{
		return doFindCallableListHelper(this, e);
	}

private:
	template <typename Tuple, std::size_t ...Indices>
	static bool doInvokePlannedArguments(const CallbackList_ & listeners, Callback & callback,
		Tuple & arguments, IndexSequence<Indices...>)
	{
		return listeners.doInvokeCallback(callback, std::get<Indices>(arguments)...);
	}

	void doSetListenerSelection(const Event & event, std::shared_ptr<ListenerSelection> stored)
	{
		std::lock_guard<Mutex> lockGuard(listenerMutex);
		stored.swap(listenerOrderingMap[event]);
		// The stored argument is destroyed after the lock guard.
	}

	static OrderingMap cloneOrderingMap(const OrderingMap & source)
	{
		OrderingMap result;
		for(const auto & item : source) {
			result.emplace(item.first, std::make_shared<ListenerSelection>(*item.second));
		}
		return result;
	}

	// template helper to avoid code duplication in doFindCallableList
	template <typename T>
	static auto doFindCallableListHelper(T * self, const Event & e)
		-> typename std::conditional<std::is_const<T>::value, const CallbackList_ *, CallbackList_ *>::type
	{
		std::lock_guard<Mutex> lockGuard(self->listenerMutex);

		auto it = self->eventCallbackListMap.find(e);
		if(it != self->eventCallbackListMap.end()) {
			return &it->second;
		}
		else {
			return nullptr;
		}
	}

private:
	// Mixin related
	struct DoMixinBeforeDispatch
	{
		template <typename T, typename Self, typename ...A>
		static auto forEach(const Self * self, A && ...args)
			-> typename std::enable_if<HasFunctionMixinBeforeDispatch<T, A...>::value, bool>::type {
			return static_cast<const T *>(self)->mixinBeforeDispatch(std::forward<A>(args)...);
		}

		template <typename T, typename Self, typename ...A>
		static auto forEach(const Self * /*self*/, A && ... /*args*/)
			-> typename std::enable_if<! HasFunctionMixinBeforeDispatch<T, A...>::value, bool>::type {
			return true;
		}
	};

private:
	Map eventCallbackListMap;
	OrderingMap listenerOrderingMap;
	mutable Mutex listenerMutex;
};


} //namespace internal_

template <
	typename Event_,
	typename Prototype_,
	typename Policies_ = DefaultPolicies
>
class EventDispatcher : public internal_::InheritMixins<
		internal_::EventDispatcherBase<Event_, Prototype_, Policies_, void>,
		typename internal_::SelectMixins<Policies_, internal_::HasTypeMixins<Policies_>::value >::Type
	>::Type, public TagEventDispatcher
{
private:
	using super = typename internal_::InheritMixins<
		internal_::EventDispatcherBase<Event_, Prototype_, Policies_, void>,
		typename internal_::SelectMixins<Policies_, internal_::HasTypeMixins<Policies_>::value >::Type
	>::Type;

public:
	using super::super;
	
	friend void swap(EventDispatcher & first, EventDispatcher & second) noexcept {
		first.swap(second);
	}
};


} //namespace eventpp


#endif
