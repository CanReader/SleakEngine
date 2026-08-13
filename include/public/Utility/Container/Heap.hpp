#ifndef _HEAP_H_
#define _HEAP_H_

namespace Sleak
{
    /**
 * @class Heap
 * @brief Implements a binary heap data structure for efficient priority queue operations.
 *
 * The Heap class maintains a collection of elements with a specific ordering property
 * (min-heap or max-heap). It is suitable for scenarios where you need to efficiently
 * retrieve the minimum or maximum element. Use this class when you need to implement
 * priority queues, scheduling algorithms, or heap-based sorting.
 *
 * Example Use Cases:
 * - Implementing a priority queue for task scheduling.
 * - Finding the kth largest element in a data stream.
 * - Implementing Dijkstra's shortest path algorithm.
 * - Heap sort implementation.
 *
 * Implementation Details:
 * - Uses an array-based representation to store the heap elements.
 * - Provides methods for inserting elements and extracting the minimum or maximum element.
 * - Maintains the heap property after each operation.
 */
    template <typename T, bool isMinHeap = true>
    class Heap
    {
    private:
        T* data;
        size_t capacity;
        size_t size;

        void heapifyUp(size_t index)
        {
            while (index > 0)
            {
                size_t parent = (index - 1) / 2;
                if ((isMinHeap && data[index] < data[parent]) || (!isMinHeap && data[index] > data[parent]))
                {
                    swap(data[index], data[parent]);
                    index = parent;
                }
                else
                    break;
            }
        }

        void heapifyDown(size_t index)
        {
            while (true)
            {
                size_t left = 2 * index + 1;
                size_t right = 2 * index + 2;
                size_t target = index;

                if (left < size && ((isMinHeap && data[left] < data[target]) || (!isMinHeap && data[left] > data[target])))
                    target = left;

                if (right < size && ((isMinHeap && data[right] < data[target]) || (!isMinHeap && data[right] > data[target])))
                    target = right;

                if (target != index)
                {
                    swap(data[index], data[target]);
                    index = target;
                }
                else
                    break;
            }
        }

        void swap(T& a, T& b)
        {
            T temp = a;
            a = b;
            b = temp;
        }

        void resize()
        {
            capacity *= 2;
            T* newData = new T[capacity];
            for (size_t i = 0; i < size; ++i)
                newData[i] = data[i];
            delete[] data;
            data = newData;
        }

    public:
        Heap(size_t initCapacity = 16) : size(0), capacity(initCapacity)
        {
            data = new T[capacity];
        }

        ~Heap()
        {
            delete[] data;
        }

        void push(const T& value)
        {
            if (size == capacity)
                resize();
            data[size] = value;
            heapifyUp(size);
            ++size;
        }

        T pop()
        {
            if (size == 0)
                throw "Heap is empty";

            T top = data[0];
            data[0] = data[--size];
            heapifyDown(0);
            return top;
        }

        T top() const
        {
            if (size == 0)
                throw "Heap is empty";
            return data[0];
        }

        size_t getSize() const { return size; }
    };
}

#endif // _HEAP_H_
