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

#ifndef CALLBACKLIST_H_588722158669
#define CALLBACKLIST_H_588722158669

#include "eventpolicies.h"

#include <functional>
#include <mutex>
#include <cassert>
#include <vector>
#include <algorithm>
#include <utility>

namespace eventpp {

namespace internal_ {

template <typename Event, typename Prototype, typename Policies, typename MixinRoot>
class EventDispatcherBase;

template <
	typename Prototype,
	typename PoliciesType
>
class CallbackListBase;

template <
	typename PoliciesType,
	typename ReturnType, typename ...Args
>
class CallbackListBase<
	ReturnType (Args...),
	PoliciesType
>
{
private:
	using Policies = PoliciesType;

	using Threading = typename SelectThreading<Policies, HasTypeThreading<Policies>::value>::Type;

	using Callback_ = typename SelectCallback<
		Policies,
		HasTypeCallback<Policies>::value,
		std::function<ReturnType (Args...)>
	>::Type;

	using CanContinueInvoking = typename SelectCanContinueInvoking<
		Policies, HasFunctionCanContinueInvoking<Policies, Args...>::value
	>::Type;
	using Metadata_ = typename SelectListenerMetadata<Policies, HasTypeListenerMetadata<Policies>::value>::Type;

	struct Node;
	using NodePtr = std::shared_ptr<Node>;

	struct Node
	{
		using Counter = unsigned int;

		Node(const Callback_ & callback, const Counter counter,
			const std::shared_ptr<const Metadata_> & metadata = nullptr)
			: callback(callback), counter(counter), metadata(metadata)
		{
		}

		NodePtr previous;
		NodePtr next;
		Callback_ callback;
		// Traversal reads the removal marker outside the list mutex.
		typename Threading::template Atomic<Counter> counter;
		std::shared_ptr<const Metadata_> metadata;
	};

	class Handle_ : public std::weak_ptr<Node>
	{
	private:
		using super = std::weak_ptr<Node>;

	public:
		using super::super;

		operator bool () const noexcept {
			return ! this->expired();
		}
	};

	using Counter = typename Node::Counter;
	enum : Counter {
		removedCounter = 0
	};

public:
	using Callback = Callback_;
	using Handle = Handle_;
	using Mutex = typename Threading::Mutex;
	using ListenerMetadata = Metadata_;

	struct ListenerInfo
	{
		Handle handle;
		ListenerMetadata metadata;
	};
	using ListenerList = std::vector<ListenerInfo>;
	using ListenerOrder = std::vector<Handle>;

public:
	CallbackListBase() noexcept
		:
			head(),
			tail(),
			mutex(),
			currentCounter(0)
	{
	}

	CallbackListBase(const CallbackListBase & other)
		: CallbackListBase()
	{
		cloneFrom(other.head);
	}

	CallbackListBase(CallbackListBase && other) noexcept
		: CallbackListBase()
	{
		swap(other);
	}

	// If we use pass by value idiom and omit the 'this' check,
	// when assigning to self there is a deep copy which is inefficient.
	CallbackListBase & operator = (const CallbackListBase & other) {
		if(this != &other) {
			CallbackListBase copied(other);
			swap(copied);
		}
		return *this;
	}

	CallbackListBase & operator = (CallbackListBase && other) noexcept {
		if(this != &other) {
			doFreeAllNodes();

			head = std::move(other.head);
			tail = std::move(other.tail);
			currentCounter = other.currentCounter.load();
		}
		return *this;
	}

	~CallbackListBase()	{
		// Don't lock mutex here since it may throw exception

		doFreeAllNodes();
	}
	
	void swap(CallbackListBase & other) noexcept {
		using std::swap;
		
		swap(head, other.head);
		swap(tail, other.tail);

		const auto value = currentCounter.load();
		currentCounter.exchange(other.currentCounter.load());
		other.currentCounter.exchange(value);
	}

	bool empty() const {
		std::lock_guard<Mutex> lockGuard(mutex);
		return ! head;
	}

	operator bool() const {
		return ! empty();
	}

	Handle append(const Callback & callback)
	{
		return doAppend(callback, nullptr);
	}

	Handle append(const Callback & callback, const ListenerMetadata & metadata)
	{
		return doAppend(callback, std::make_shared<const ListenerMetadata>(metadata));
	}

private:
	Handle doAppend(const Callback & callback, const std::shared_ptr<const ListenerMetadata> & metadata)
	{
		NodePtr node(doAllocateNode(callback, metadata));

		std::lock_guard<Mutex> lockGuard(mutex);

		if(head) {
			node->previous = tail;
			tail->next = node;
			tail = node;
		}
		else {
			head = node;
			tail = node;
		}

		return Handle(node);
	}

public:
	Handle prepend(const Callback & callback)
	{
		return doPrepend(callback, nullptr);
	}

	Handle prepend(const Callback & callback, const ListenerMetadata & metadata)
	{
		return doPrepend(callback, std::make_shared<const ListenerMetadata>(metadata));
	}

private:
	Handle doPrepend(const Callback & callback, const std::shared_ptr<const ListenerMetadata> & metadata)
	{
		NodePtr node(doAllocateNode(callback, metadata));

		std::lock_guard<Mutex> lockGuard(mutex);

		if(head) {
			node->next = head;
			head->previous = node;
			head = node;
		}
		else {
			head = node;
			tail = node;
		}

		return Handle(node);
	}

public:
	Handle insert(const Callback & callback, const Handle & before)
	{
		return doInsertCallback(callback, before, nullptr);
	}

	Handle insert(const Callback & callback, const Handle & before, const ListenerMetadata & metadata)
	{
		return doInsertCallback(callback, before, std::make_shared<const ListenerMetadata>(metadata));
	}

private:
	Handle doInsertCallback(const Callback & callback, const Handle & before,
		const std::shared_ptr<const ListenerMetadata> & metadata)
	{
		// Disable this assertion because it's too slow in debug mode.
		//assert(before.expired() || ownsHandle(before));

		NodePtr beforeNode = before.lock();
		if(beforeNode) {
			NodePtr node(doAllocateNode(callback, metadata));

			std::lock_guard<Mutex> lockGuard(mutex);

			doInsert(node, beforeNode);

			return Handle(node);
		}

		return doAppend(callback, metadata);
	}

public:
	bool setListenerMetadata(const Handle & handle, const ListenerMetadata & metadata)
	{
		auto stored = std::make_shared<const ListenerMetadata>(metadata);
		std::lock_guard<Mutex> lockGuard(mutex);
		auto node = doFindMemberNode(handle);
		if(! node) { return false; }
		stored.swap(node->metadata);
		return true;
	}

	bool getListenerMetadata(const Handle & handle, ListenerMetadata & metadata) const
	{
		std::shared_ptr<const ListenerMetadata> stored;
		{
			std::lock_guard<Mutex> lockGuard(mutex);
			auto node = doFindMemberNode(handle);
			if(! node) { return false; }
			stored = node->metadata;
		}
		// Run application-defined assignment outside the list mutex.
		if(stored) {
			metadata = *stored;
		}
		else {
			const ListenerMetadata empty;
			metadata = empty;
		}
		return true;
	}

	template <typename Updater>
	bool updateListenerMetadata(const Handle & handle, Updater && updater)
	{
		NodePtr node;
		std::shared_ptr<const ListenerMetadata> expected;
		{
			std::lock_guard<Mutex> lockGuard(mutex);
			node = doFindMemberNode(handle);
			if(! node) { return false; }
			expected = node->metadata;
		}
		for(;;) {
			auto updated = expected ? std::make_shared<ListenerMetadata>(*expected)
				: std::make_shared<ListenerMetadata>();
			updater(*updated);
			std::shared_ptr<const ListenerMetadata> stored = std::move(updated);
			std::shared_ptr<const ListenerMetadata> latest;
			{
				std::lock_guard<Mutex> lockGuard(mutex);
				if(node->counter.load(std::memory_order_relaxed) == removedCounter) { return false; }
				if(node->metadata == expected) {
					stored.swap(node->metadata);
					return true;
				}
				latest = node->metadata;
			}
			// Copies, updater calls and destruction of superseded values stay outside mutex.
			expected = std::move(latest);
		}
	}

	bool remove(const Handle & handle)
	{
		// Disable this assertion because it's too slow in debug mode.
		//assert(handle.expired() || ownsHandle(handle));

		// It looks like the lock can be put inside the `if` below,
		// but that doesn't work in multi-threading and cause related unit tests fail.
		std::lock_guard<Mutex> lockGuard(mutex);

		auto node = handle.lock();
		if(node) {
			doFreeNode(node);
			return true;
		}

		return false;
	}

	bool ownsHandle(const Handle & handle) const
	{
		std::lock_guard<Mutex> lockGuard(mutex);

		auto node = handle.lock();
		if(node) {
			while(node->previous) {
				node = node->previous;
			}
			return node == head;
		}

		return false;
	}

	template <typename Func>
	void forEach(Func && func) const
	{
		doForEachIf([&func, this](NodePtr & node) -> bool {
			doForEachInvoke<void>(func, node);
			return true;
		});
	}

	template <typename Func>
	bool forEachIf(Func && func) const
	{
		return doForEachIf([&func, this](NodePtr & node) -> bool {
			return doForEachInvoke<bool>(func, node);
		});
	}

#if !defined(__GNUC__) || __GNUC__ >= 5
	void operator() (Args ...args) const
	{
		forEachIf([&args...](Callback & callback) -> bool {
			// We can't use std::forward here, because if we use std::forward,
			// for arg that is passed by value, and the callback prototype accepts it by value,
			// std::forward will move it and may cause the original value invalid.
			// That happens on any value-to-value passing, no matter the callback moves it or not.

			callback(args...);
			return CanContinueInvoking::canContinueInvoking(args...);
		});
	}
#else
	// This is a patch version for GCC 4. It inlines the unrolled doForEachIf.
	// GCC 4.8.3 doesn't supporting parameter pack catpure in lambda, see,
	// https://github.com/wqking/eventpp/issues/19
	// This is a compromised patch for GCC 4, it may be not maintained or updated unless there are bugs.
	// We don't use the patch as main code because the patch generates longer code, and duplicated with doForEachIf.
	void operator() (Args ...args) const
	{
		NodePtr node;

		{
			std::lock_guard<Mutex> lockGuard(mutex);
			node = head;
		}

		const Counter counter = currentCounter.load(std::memory_order_acquire);

		while(node) {
			const Counter nodeCounter = node->counter.load(std::memory_order_relaxed);
			if(nodeCounter != removedCounter && counter >= nodeCounter) {
				node->callback(args...);
				if(! CanContinueInvoking::canContinueInvoking(args...)) {
					break;
				}
			}

			{
				std::lock_guard<Mutex> lockGuard(mutex);
				node = node->next;
			}
		}
	}
#endif

private:
	template <typename Event, typename Prototype, typename P, typename MixinRoot>
	friend class EventDispatcherBase;

	// The caller holds mutex. Only inspect links belonging to this list.
	NodePtr doFindMemberNode(const Handle & handle) const
	{
		auto node = handle.lock();
		if(node) {
			for(auto member = head; member; member = member->next) {
				if(member == node) { return member; }
			}
		}
		return nullptr;
	}

	ListenerList doGetListeners() const
	{
		ListenerList listeners;
		std::lock_guard<Mutex> lockGuard(mutex);
		// Reserve once: growing the vector can repeatedly copy application metadata.
		listeners.reserve(doCountListenersLocked());
		for(NodePtr node = head; node; node = node->next) {
			listeners.push_back(ListenerInfo { Handle(node),
				node->metadata ? *node->metadata : ListenerMetadata() });
		}
		return listeners;
	}

	std::size_t doCountListeners() const
	{
		std::lock_guard<Mutex> lockGuard(mutex);
		return doCountListenersLocked();
	}

	// The caller holds mutex, so no ownership copies are needed during this traversal.
	std::size_t doCountListenersLocked() const
	{
		std::size_t count = 0;
		for(const Node * node = head.get(); node; node = node->next.get()) { ++count; }
		return count;
	}

	void doInvokeInOrder(const ListenerList & listeners, const ListenerOrder & order,
		typename std::add_lvalue_reference<Args>::type ...args) const
	{
		doInvokeSelected(listeners, order,
			[](const Handle & handle) -> const Handle & { return handle; },
			[&](Callback & callback, const Handle &) {
				return doInvokeCallback(callback, args...);
			});
	}

	bool doInvokeCallback(Callback & callback,
		typename std::add_lvalue_reference<Args>::type ...args) const
	{
		callback(args...);
		return CanContinueInvoking::canContinueInvoking(args...);
	}

	template <typename Selection, typename GetHandle, typename Invoke>
	void doInvokeSelected(const ListenerList & listeners, Selection & selection,
		GetHandle && getHandle, Invoke && invoke) const
	{
		// Accept only handles in the snapshot, and invoke each subscription at most once.
		if(selection.empty() || listeners.empty()) { return; }
		using WeakNode = std::weak_ptr<Node>;
		using HandleState = std::pair<WeakNode, bool>;
		const std::owner_less<WeakNode> less;
		std::vector<HandleState> remaining;
		remaining.reserve(listeners.size());
		for(const auto & listener : listeners) {
			remaining.emplace_back(listener.handle, false);
		}
		std::sort(remaining.begin(), remaining.end(),
			[&less](const HandleState & a, const HandleState & b) { return less(a.first, b.first); });
		for(auto & entry : selection) {
			const Handle & handle = getHandle(entry);
			auto it = std::lower_bound(remaining.begin(), remaining.end(), handle,
				[&less](const HandleState & state, const WeakNode & value) { return less(state.first, value); });
			if(it == remaining.end() || less(handle, it->first) || it->second) {
				continue;
			}
			// Keep owners in sorted order after invocation, including expired owners.
			it->second = true;
			NodePtr node;
			{
				std::lock_guard<Mutex> lockGuard(mutex);
				node = handle.lock();
				if(node && node->counter.load(std::memory_order_relaxed) == removedCounter) {
					node.reset();
				}
			}
			if(node) {
				if(! invoke(node->callback, entry)) {
					return;
				}
			}
		}
	}

	template <typename F>
	bool doForEachIf(F && f) const
	{
		NodePtr node;

		{
			std::lock_guard<Mutex> lockGuard(mutex);
			node = head;
		}

		const Counter counter = currentCounter.load(std::memory_order_acquire);

		while(node) {
			const Counter nodeCounter = node->counter.load(std::memory_order_relaxed);
			if(nodeCounter != removedCounter && counter >= nodeCounter) {
				if(! f(node)) {
					return false;
				}
			}

			{
				std::lock_guard<Mutex> lockGuard(mutex);
				node = node->next;
			}
		}

		return true;
	}

	template <typename RT, typename Func>
	auto doForEachInvoke(Func && func, NodePtr & node) const
		-> typename std::enable_if<CanInvoke<Func, Handle, Callback &>::value, RT>::type
	{
		return func(Handle(node), node->callback);
	}

	template <typename RT, typename Func>
	auto doForEachInvoke(Func && func, NodePtr & node) const
		-> typename std::enable_if<CanInvoke<Func, Callback &>::value, RT>::type
	{
		return func(node->callback);
	}

	void doInsert(NodePtr & node, NodePtr & beforeNode)
	{
		node->previous = beforeNode->previous;
		node->next = beforeNode;
		if(beforeNode->previous) {
			beforeNode->previous->next = node;
		}
		beforeNode->previous = node;

		if(beforeNode == head) {
			head = node;
		}
	}
	
	NodePtr doAllocateNode(const Callback & callback,
		const std::shared_ptr<const ListenerMetadata> & metadata = nullptr)
	{
		return std::make_shared<Node>(callback, getNextCounter(), metadata);
	}
	
	void doFreeNode(NodePtr & node)
	{
		if(node->next) {
			node->next->previous = node->previous;
		}
		if(node->previous) {
			node->previous->next = node->next;
		}

		// Mark it as deleted, this must be before the assignment of head and tail below,
		// because node can be a reference to head or tail, and after the assignment, node
		// can be null pointer.
		node->counter.store(removedCounter, std::memory_order_relaxed);

		if(head == node) {
			head = node->next;
		}
		if(tail == node) {
			tail = node->previous;
		}

		// don't modify node->previous or node->next
		// because node may be still used in a loop.
	}

	void doFreeAllNodes() {
		NodePtr node = head;
		head.reset();
		while(node) {
			NodePtr next = node->next;
			node->previous.reset();
			node->next.reset();
			node = next;
		}
		node.reset();
	}

	Counter getNextCounter()
	{
		Counter result = ++currentCounter;;
		if(result == 0) { // overflow, let's reset all nodes' counters.
			{
				std::lock_guard<Mutex> lockGuard(mutex);
				NodePtr node = head;
				while(node) {
					node->counter.store(1, std::memory_order_relaxed);
					node = node->next;
				}
			}
			result = ++currentCounter;
		}

		return result;
	}
	
	void cloneFrom(const NodePtr & fromHead) {
		NodePtr fromNode(fromHead);
		NodePtr node;
		const Counter counter = getNextCounter();
		while(fromNode) {
			const NodePtr nextNode(std::make_shared<Node>(fromNode->callback, counter, fromNode->metadata));

			nextNode->previous = node;

			if(node) {
				node->next = nextNode;
			}
			else {
				node = nextNode;
				head = node;
			}
		
			node = nextNode;
			fromNode = fromNode->next;
		}

		tail = node;
	}

private:
	NodePtr head;
	NodePtr tail;
	mutable Mutex mutex;
	typename Threading::template Atomic<Counter> currentCounter;

};


} //namespace internal_


template <
	typename Prototype_,
	typename Policies_ = DefaultPolicies
>
class CallbackList : public internal_::CallbackListBase<Prototype_, Policies_>, public TagCallbackList
{
private:
	using super = internal_::CallbackListBase<Prototype_, Policies_>;
	
public:
	using super::super;
	
	friend void swap(CallbackList & first, CallbackList & second) noexcept {
		first.swap(second);
	}
};


} //namespace eventpp


#endif
