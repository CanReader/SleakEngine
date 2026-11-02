#ifndef _QUEUE_H_
#define _QUEUE_H_

#include "List.hpp"
#include <algorithm>
#include <sstream>
#include <utility>

namespace Sleak
{
	
	/**
	 * @class Queue
	 * @brief Implements a FIFO (First-In, First-Out) queue data structure.
	 *
	 * The Queue class maintains a collection of elements in the order they were added. It
	 * is suitable for scenarios where you need to process elements in a first-come,
	 * first-served manner. Use this class when you need to implement task queues,
	 * message queues, or breadth-first search algorithms.
	 *
	 * Example Use Cases:
	 * - Implementing a task queue for background processing.
	 * - Managing message queues in a network communication system.
	 * - Performing breadth-first search in graph algorithms.
	 * - Simulating real-world queues, such as waiting lines.
	 *
	 * Implementation Details:
	 * - Uses an underlying List plus a head index; pop is amortized O(1).
	 * - Provides methods for enqueueing (adding) and dequeueing (removing) elements.
	 * - Maintains the FIFO order of elements.
	 * @ingroup utility
	 */
	template <typename T>
	class Queue {
	public:
	    Queue() = default;
	    Queue(const Queue&) = default;
	    Queue& operator=(const Queue&) = default;

	    /// Takes other's elements, leaving it empty.
	    Queue(Queue&& other) noexcept
	        : data(std::move(other.data)), head(std::exchange(other.head, 0)) {}

	    /// Replaces this queue with other's elements, leaving other empty.
	    Queue& operator=(Queue&& other) noexcept {
	        if (this != &other) {
	            data = std::move(other.data);
	            head = std::exchange(other.head, 0);
	        }
	        return *this;
	    }

	    /// Adds value to the back of the queue.
	    void push(const T& value) {
	        data.add(value);
	    }

	    /// Moves value onto the back of the queue.
	    void push(T&& value) {
	        data.add(std::move(value));
	    }

	    /// Removes and returns the front element; throws if empty.
	    T pop() {
	        if (isEmpty()) {
	            throw Sleak::EmptyContainerException();
	        }

	        T* slots = data.GetData();
	        T front = std::move(slots[head]);
	        slots[head] = T();
	        ++head;
	        compactIfSparse();

	        return front;
	    }

	    /// Front element without removing it; throws if empty.
	    T& front() {
	        if (isEmpty()) {
	            throw Sleak::EmptyContainerException();
	        }
	        return data.GetData()[head];
	    }

	    const T& front() const {
	        if (isEmpty()) {
	            throw Sleak::EmptyContainerException();
	        }
	        return data.GetData()[head];
	    }

	    /// Reverses the queue's element order in place.
	    const void reverse() {
	        std::reverse(begin(), end());
	    }

	    bool isEmpty() const {
	        return size() == 0;
	    }

	    size_t size() const { return data.GetSize() - head; }

	    void clear() {
	        data.clear();
	        head = 0;
	    }

	    /// Copies out the queued elements, front-to-back.
	    List<T> GetData() {
	        List<T> out;
	        out.resize(size());
	        for (const T& value : *this) {
	            out.add(value);
	        }
	        return out;
	    }

	    // Iterators
	    T* begin() { return data.begin() + head; }
	    const T* begin() const { return data.begin() + head; }
	    T* end() { return data.end(); }
	    const T* end() const { return data.end(); }

	private:
	    /// Slides live elements to the front once popped slots are the majority.
	    void compactIfSparse() {
	        const size_t live = size();
	        if (head < live) {
	            return;
	        }
	        if (live == 0) {
	            clear();
	            return;
	        }

	        T* slots = data.GetData();
	        for (size_t i = 0; i < live; ++i) {
	            slots[i] = std::move(slots[head + i]);
	        }
	        while (data.GetSize() > live) {
	            data.erase(data.GetSize() - 1);
	        }
	        head = 0;
	    }

	    List<T> data;
	    size_t head = 0;
	};
}

#endif // _QUEUE_H_