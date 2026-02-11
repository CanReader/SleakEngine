#ifndef _QUEUE_H_
#define _QUEUE_H_

#include "List.hpp"
#include <sstream>

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
	 * - Uses an underlying List to store the queue elements.
	 * - Provides methods for enqueueing (adding) and dequeueing (removing) elements.
	 * - Maintains the FIFO order of elements.
	 */
	template <typename T>
	class Queue {
	public:
	    Queue() = default;
	
	    void push(const T& value) {
	        data.add(value);
	    }
	
	    T pop() {
	        if (isEmpty()) {
	            throw Sleak::EmptyContainerException();
	        }
	
	        T front = std::move(data[0]);
	
	        data.erase(0);
	
	        return front;
	    }
	
	    T& front() {
	        if (isEmpty()) {
	            throw Sleak::EmptyContainerException();
	        }
	        return data[0];
	    }
	
	    const T& front() const {
	        if (isEmpty()) {
	            throw Sleak::EmptyContainerException();
	        }
	        return data[0];
	    }

		const void reverse() {
			data.reverse();
		}
	
	    bool isEmpty() const {
	        return data.GetSize() == 0;
	    }
	
	    size_t size() const { return data.GetSize(); }
	
	    void clear() {
	        data.clear();
	    }

		List<T> GetData() { return data; }

		// Iterators
		T* begin() { return data.begin(); }
		const T* begin() const { return data.begin(); }
		T* end() { return data.end(); }
		const T* end() const { return data.end(); }

	private:
	    List<T> data;
	};
}

#endif // _QUEUE_H_