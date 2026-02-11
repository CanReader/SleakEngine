#ifndef _STACK_H_
#define _STACK_H_

#include "List.hpp" // Assuming you have your List class

namespace Sleak
{
	/**
	 * @class Stack
	 * @brief Implements a LIFO (Last-In, First-Out) stack data structure.
	 *
	 * The Stack class maintains a collection of elements with the most recently added element
	 * at the top. It is suitable for scenarios where you need to process elements in a
	 * last-come, first-served manner. Use this class when you need to implement function
	 * call stacks, expression evaluation, or depth-first search algorithms.
	 *
	 * Example Use Cases:
	 * - Implementing a function call stack in a compiler or interpreter.
	 * - Evaluating arithmetic expressions with parentheses.
	 * - Performing depth-first search in graph algorithms.
	 * - Implementing undo/redo functionality in an editor.
	 *
	 * Implementation Details:
	 * - Uses an underlying List to store the stack elements.
	 * - Provides methods for pushing (adding) and popping (removing) elements.
	 * - Maintains the LIFO order of elements.
	 */
	template <typename T>
	class Stack {
	public:
	    Stack() = default;
	
	    void push(const T& value) {
	        data.add(value);
	    }
	
	    void pop() {
	        if (isEmpty()) {
	            throw "Stack is empty";
	        }
	
	        if (data.getSize() > 0) {  // Check if there are elements to pop
	            data.insert(data.getSize() - 1, List<T>().begin(), List<T>().end());
	            data.insert(data.getSize() - 1, List<T>().begin(), List<T>().end());
	        } else {
	            throw "Stack is empty";
	        }
	    }
	
	    T& top() {
	        if (isEmpty()) {
	            throw "Stack is empty";
	        }
	        return data[data.getSize() - 1];
	    }
	
	    const T& top() const {
	        if (isEmpty()) {
	            throw "Stack is empty";
	        }
	        return data[data.getSize() - 1];
	    }
	
	    bool isEmpty() const {
	        return data.getSize() == 0;
	    }
	
	    size_t size() const {
	        return data.getSize();
	    }
	
	    void clear() {
	        data.clear();
	    }
	
	private:
	    List<T> data;
	};
}

#endif // _STACK_H_