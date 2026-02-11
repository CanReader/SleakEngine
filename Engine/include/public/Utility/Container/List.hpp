#ifndef _LIST_H_
#define _LIST_H_

#include <Utility/Exception.hpp>
#include <algorithm>  // For std::swap
#include <functional>

namespace Sleak {
/**
 * @class List
 * @brief Implements a dynamic array-like list for storing and managing a
 * collection of elements.
 *
 * The List class provides a flexible and efficient way to store and access
 * elements in a linear order. It is suitable for scenarios where you need to
 * dynamically resize the collection or access elements by index.
 */
template <typename T>
class List {
   private:
    T* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;

    void quickSort(int low, int high,
                   std::function<bool(const T&, const T&)> compare) {
        if (low < high) {
            int pivotIndex = partition(low, high, compare);
            quickSort(low, pivotIndex - 1, compare);
            quickSort(pivotIndex + 1, high, compare);
        }
    }

    int partition(int low, int high,
                  std::function<bool(const T&, const T&)> compare) {
        T pivot = data[high];
        int i = low - 1;

        for (int j = low; j < high; j++) {
            if (compare(data[j], pivot)) {
                i++;
                std::swap(data[i], data[j]);
            }
        }

        std::swap(data[i + 1], data[high]);
        return i + 1;
    }

   public:
    List() noexcept = default;

    List(T* Data, size_t Size) : data(Data), size(Size), capacity(Size) {}

    // Copy constructor
    List(const List& other) : size(other.size), capacity(other.capacity) {
        data = new T[capacity];
        for (size_t i = 0; i < size; ++i) {
            data[i] = other.data[i];
        }
    }

    // Move constructor
    List(List&& other) noexcept
        : data(other.data), size(other.size), capacity(other.capacity) {
        other.data = nullptr;
        other.size = 0;
        other.capacity = 0;
    }

    // Copy assignment
    List& operator=(const List& other) {
        if (this != &other) {
            delete[] data;
            size = other.size;
            capacity = other.capacity;
            data = new T[capacity];
            for (size_t i = 0; i < size; ++i) {
                data[i] = other.data[i];
            }
        }
        return *this;
    }

    // Move assignment
    List& operator=(List&& other) noexcept {
        if (this != &other) {
            delete[] data;
            data = other.data;
            size = other.size;
            capacity = other.capacity;
            other.data = nullptr;
            other.size = 0;
            other.capacity = 0;
        }
        return *this;
    }

    List(std::initializer_list<T> initList) {
        for (const T& value : initList) {
            add(value);
        }
    }

    ~List() { delete[] data; }

    // Element addition
    void add(const T& value) {
        if (size == capacity) resize();
        data[size++] = value;
    }

    void add(T&& value) {
        if (size == capacity) resize();
        data[size++] = std::move(value);
    }

    void add(std::initializer_list<T> AddList) {
        for (auto&& item : AddList) {
            add(std::move(item));
        }
    }

    // Element access
    T& operator[](size_t index) {
        if (index >= size) throw Sleak::IndexOutOfBoundsException();
        return data[index];
    }

    const T& operator[](size_t index) const {
        if (index >= size) throw Sleak::IndexOutOfBoundsException();
        return data[index];
    }

    T& at(size_t index) {
        if (index >= size) throw Sleak::IndexOutOfBoundsException();
        return data[index];
    }

    const T& at(size_t index) const {
        if (index >= size) throw Sleak::IndexOutOfBoundsException();
        return data[index];
    }

    // Capacity
    size_t GetSize() const { return size; }
    size_t GetByteSize() const { return size * sizeof(T); }
    size_t GetCapacity() const { return capacity; }
    bool empty() const { return size == 0; }

    // Data access
    T* GetData() { return data; }
    const T* GetData() const { return data; }
    void* GetRawData() { return static_cast<void*>(data); }
    const void* GetRawData() const { return static_cast<const void*>(data); }

    // Iterators
    T* begin() { return data; }
    const T* begin() const { return data; }
    T* end() { return data + size; }
    const T* end() const { return data + size; }

    // Modifiers
    void clear() { size = 0; }

    void resize() {
        size_t newCapacity = (capacity == 0) ? 1 : capacity * 2;
        resize(newCapacity);
    }

    void resize(size_t newCapacity) {
        if (newCapacity <= capacity) return;

        T* newData = new T[newCapacity];
        for (size_t i = 0; i < size; ++i) {
            newData[i] = std::move(data[i]);
        }
        delete[] data;
        data = newData;
        capacity = newCapacity;
    }

    void insert(size_t index, const T& value) {
        if (index > size) throw Sleak::IndexOutOfBoundsException();
        if (size == capacity) resize();

        for (size_t i = size; i > index; --i) {
            data[i] = data[i - 1];
        }
        data[index] = value;
        ++size;
    }

    void erase(size_t index) {
        if (index >= size) throw Sleak::IndexOutOfBoundsException();

        for (size_t i = index; i < size - 1; ++i) {
            data[i] = data[i + 1];
        }
        --size;
    }

    // Operations
    void sort(std::function<bool(const T&, const T&)> compare) {
        quickSort(0, size - 1, compare);
    }

    void reverse() {
        for (size_t i = 0; i < size / 2; ++i) {
            std::swap(data[i], data[size - 1 - i]);
        }
    }

    void swap(List& other) noexcept {
        std::swap(data, other.data);
        std::swap(size, other.size);
        std::swap(capacity, other.capacity);
    }

    // Search
    T* find(std::function<bool(const T&)> predicate) {
        for (size_t i = 0; i < size; ++i) {
            if (predicate(data[i])) return &data[i];
        }
        return nullptr;
    }

    const T* find(std::function<bool(const T&)> predicate) const {
        for (size_t i = 0; i < size; ++i) {
            if (predicate(data[i])) return &data[i];
        }
        return nullptr;
    }

    int32_t indexOf(const T& value) {
        for(int i = 0; i < size; i++)
            if(data[i] == value)
                return i;
        
        return -1;
    }
};
}  // namespace Sleak

#endif  // _LIST_H_