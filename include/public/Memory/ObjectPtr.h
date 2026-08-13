#ifndef _OBJECT_PTR_H_
#define _OBJECT_PTR_H_

#include "SmartPointer.h"
#include <utility> // For std::move and std::forward
#include <type_traits> // For std::enable_if_t and std::is_base_of_v

namespace Sleak {

    template <typename T>
    class ObjectPtr : public SmartPointer<T> {
    public:
        // Default constructor
        ObjectPtr() noexcept : SmartPointer<T>(nullptr) {}

        // Constructor that takes a raw pointer
        explicit ObjectPtr(T* p) noexcept : SmartPointer<T>(p) {}

        // Constructor that takes constructor arguments for T
        template <typename... Args>
        explicit ObjectPtr(Args&&... args) : SmartPointer<T>(new T(std::forward<Args>(args)...)) {}

        // Move constructor
        ObjectPtr(ObjectPtr&& other) noexcept : SmartPointer<T>(other.release()) {}

        // Move assignment
        ObjectPtr& operator=(ObjectPtr&& other) noexcept {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }

        // Derived-to-base conversion (move)
        template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
        ObjectPtr(ObjectPtr<U>&& other) noexcept
            : SmartPointer<T>(other.release()) {}

        // Destructor
        ~ObjectPtr() { reset(); }

        // Reset the pointer
        void reset(T* newPtr = nullptr) override {
            T* oldPtr = this->ptr;
            this->ptr = newPtr;
            if (oldPtr) {
                delete oldPtr;
            }
        }

        // Release ownership of the pointer
        T* release() noexcept {
            T* temp = this->ptr;
            this->ptr = nullptr;
            return temp;
        }

        // Comparison operators
        bool operator==(const ObjectPtr& other) const noexcept { return this->ptr == other.ptr; }
        bool operator!=(const ObjectPtr& other) const noexcept { return this->ptr != other.ptr; }
        bool operator<(const ObjectPtr& other) const noexcept { return this->ptr < other.ptr; }
        bool operator<=(const ObjectPtr& other) const noexcept { return this->ptr <= other.ptr; }
        bool operator>(const ObjectPtr& other) const noexcept { return this->ptr > other.ptr; }
        bool operator>=(const ObjectPtr& other) const noexcept { return this->ptr >= other.ptr; }

        // Disable copy semantics
        ObjectPtr(const ObjectPtr&) = delete;
        ObjectPtr& operator=(const ObjectPtr&) = delete;
    };

} // namespace Sleak

#endif // _OBJECT_PTR_H_


/*
#ifdef TEMP

#include "SmartPointer.h"

namespace Sleak {

    template <typename T>
    class ObjectPtr : public SmartPointer<T> {
    public:
        // Constructor that takes constructor arguments for T
        template <typename... Args>
        explicit ObjectPtr(Args&&... args) : SmartPointer<T>(new T(std::forward<Args>(args)...)) {}

        // Constructor that takes a raw pointer (original behavior)
        explicit ObjectPtr(T* p = nullptr) : SmartPointer<T>(p) {}

        // Copy constructor
        //ObjectPtr(ObjectPtr& other) noexcept : SmartPointer<T>(other.ptr) {}
        ObjectPtr(const ObjectPtr&) = delete;

        // Copy assignment
        //ObjectPtr& operator=(ObjectPtr& other) noexcept {}
        ObjectPtr& operator=(const ObjectPtr&) = delete;

        // Move constructor
        ObjectPtr(ObjectPtr&& other) noexcept : SmartPointer<T>(other.ptr) {
            other.ptr = nullptr;
        }

        // Move assignment
        ObjectPtr& operator=(ObjectPtr&& other) noexcept {
            if (this != &other) {
                this->reset();
                this->ptr = other.ptr;
                other.ptr = nullptr;
            }
            return *this;
        }

        // Derived-to-base conversion (move)
        template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
        ObjectPtr(ObjectPtr<U>&& other) noexcept
            : SmartPointer<T>(other.release()) {}

        // Destructor
        ~ObjectPtr() { this->reset(); }

        // Reset the pointer
        void reset(T* newPtr = nullptr) override {
            if (this->ptr != nullptr)
                delete this->ptr;
            this->ptr = newPtr;
        }

        // Release ownership of the pointer
        T* release() {
            T* temp = this->ptr;
            this->ptr = nullptr;
            return temp;
        }
    };

} // namespace Sleak

#endif // _OBJECT_PTR_H_

*/