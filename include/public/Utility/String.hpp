#ifndef _STRING_H_
#define _STRING_H_

#include <iostream> // For std::ostream (optional)
#include <Utility/Exception.hpp>

namespace Sleak {

    /**
     * @class String
     * @brief A dynamic string class, similar to std::string but STL-agnostic.
     *
     * This class provides a flexible and efficient way to manage strings of characters.
     * It is designed to be independent of the Standard Template Library (STL) and can be
     * used in environments where STL is not available or desired.
     *
     * Example Use Cases:
     * - Storing and manipulating text in game engines or embedded systems.
     * - Implementing custom string processing algorithms.
     * - Learning about string implementation internals.
     *
     * Implementation Details:
     * - Uses a dynamically allocated array to store characters.
     * - Provides a wide range of methods for string manipulation,
     * including concatenation, comparison, searching, and substring extraction.
     * - Automatically manages memory allocation and resizing.
     */
    class String {
    public:
        // Constructors
        String() noexcept : data_(nullptr), size_(0), capacity_(0) {}

        String(const char* str) : String(str, strlen(str)) {}

        String(const char* str, size_t len) : size_(len), capacity_(len + 1) {
            data_ = new char[capacity_];
            memcpy(data_, str, len);
            data_[len] = '\0';
        }

        String(const String& other) : size_(other.size_), capacity_(other.capacity_) {
            data_ = new char[capacity_];
            memcpy(data_, other.data_, size_ + 1);
        }

        String(String&& other) noexcept : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }

        // Destructor
        ~String() {
            deletedata_;
        }

        // Assignment operators
        String& operator=(const String& other) {
            if (this != &other) {
                if (capacity_ < other.size_ + 1) {
                    deletedata_;
                    capacity_ = other.capacity_;
                    data_ = new char[capacity_];
                }
                size_ = other.size_;
                memcpy(data_, other.data_, size_ + 1);
            }
            return *this;
        }

        String& operator=(String&& other) noexcept {
            if (this != &other) {
                deletedata_;
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.data_ = nullptr;
                other.size_ = 0;
                other.capacity_ = 0;
            }
            return *this;
        }

        // Element access
        char& operator[](size_t index) {
            if (index >= size_) {
                throw IndexOutOfBoundsException("String index out of bounds");
            }
            return data_[index];
        }
        
        const char& operator[](size_t index) const {
            if (index >= size_) {
                throw IndexOutOfBoundsException("String index out of bounds");
            }
            return data_[index];
        }

        // Capacity
        size_t size() const { return size_; }
        size_t length() const { return size_; }
        size_t capacity() const { return capacity_; }
        bool empty() const { return size_ == 0; }

        // Modifiers
        void clear() {
            size_ = 0;
            data_[0] = '\0';
        }

        void resize(size_t newSize, char ch = '\0') {
            if (newSize > capacity_) {
                reserve(newSize + 1);
            }
            if (newSize > size_) {
                memset(data_ + size_, ch, newSize - size_);
            }
            size_ = newSize;
            data_[size_] = '\0';
        }

        void reserve(size_t newCapacity) {
            if (newCapacity > capacity_) {
                char* newData = new char[newCapacity];
                memcpy(newData, data_, size_ + 1);
                deletedata_;
                data_ = newData;
                capacity_ = newCapacity;
            }
        }

        String& append(const String& other) {
            return append(other.data_, other.size_);
        }

        String& append(const char* str) {
            return append(str, strlen(str));
        }

        String& append(const char* str, size_t len) {
            if (size_ + len > capacity_) {
                reserve(size_ + len + 1);
            }
            memcpy(data_ + size_, str, len);
            size_ += len;
            data_[size_] = '\0';
            return *this;
        }

        // String operations
        const char* c_str() const { return data_; }

        int compare(const String& other) const {
            return strcmp(data_, other.data_);
        }

        size_t find(const String& str, size_t pos = 0) const {
            const char* result = strstr(data_ + pos, str.data_);
            return result ? result - data_ : npos;
        }

        String substr(size_t pos = 0, size_t len = npos) const {
            if (pos > size_) {
                throw IndexOutOfBoundsException("String substring: pos out of bounds");
            }
            len = std::min(len, size_ - pos);
            return String(data_ + pos, len);
        }

        // Operators
        String& operator+=(const String& other) {
            return append(other);
        }

        String& operator+=(const char* str) {
            return append(str);
        }

        String& operator+=(char ch) {
            return append(&ch, 1);
        }

        friend String operator+(const String& lhs, const String& rhs) {
            String result(lhs);
            result += rhs;
            return result;
        }

        friend String operator+(const String& lhs, const char* rhs) {
            String result(lhs);
            result += rhs;
            return result;
        }

        friend String operator+(const char* lhs, const String& rhs) {
            String result(lhs);
            result += rhs;
            return result;
        }

        friend bool operator==(const String& lhs, const String& rhs) {
            return lhs.compare(rhs) == 0;
        }

        friend bool operator!=(const String& lhs, const String& rhs) {
            return lhs.compare(rhs) != 0;
        }

        friend bool operator<(const String& lhs, const String& rhs) {
            return lhs.compare(rhs) < 0;
        }

        friend bool operator<=(const String& lhs, const String& rhs) {
            return lhs.compare(rhs) <= 0;
        }

        friend bool operator>(const String& lhs, const String& rhs) {
            return lhs.compare(rhs) > 0;
        }

        friend bool operator>=(const String& lhs, const String& rhs) {
            return lhs.compare(rhs) >= 0;
        }

        friend std::ostream& operator<<(std::ostream& os, const String& str) {
            os << str.c_str();
            return os;
        }

    private:
        char* data_;
        size_t size_;
        size_t capacity_;

        static const size_t npos = static_cast<size_t>(-1);

        void deletedata_() {
            delete[] data_;
            data_ = nullptr;
        }
    };
}

#endif // _STRING_H_