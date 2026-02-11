#ifndef _WEAK_PTR_H_
#define _WEAK_PTR_H_

#include "RefPtr.h"

namespace Sleak {

    template <typename T>
    class WeakPtr {
    private:
        T* ptr;          // Raw pointer to the observed object
        size_t* refCount; // Pointer to the reference count

    public:
        // Default constructor
        WeakPtr() : ptr(nullptr), refCount(nullptr) {}

        // Constructor from RefPtr
        WeakPtr(const RefPtr<T>& refPtr)
            : ptr(refPtr.get()), refCount(refPtr.refCount) {}

        // Copy constructor
        WeakPtr(const WeakPtr& other)
            : ptr(other.ptr), refCount(other.refCount) {}

        // Destructor
        ~WeakPtr() = default;

        // Copy assignment
        WeakPtr& operator=(const WeakPtr& other) {
            if (this != &other) {
                ptr = other.ptr;
                refCount = other.refCount;
            }
            return *this;
        }

        // Assignment from RefPtr
        WeakPtr& operator=(const RefPtr<T>& refPtr) {
            ptr = refPtr.get();
            refCount = refPtr.refCount;
            return *this;
        }

        // Lock the WeakPtr to create a RefPtr
        RefPtr<T> lock() const {
            if (refCount && *refCount > 0) {
                return RefPtr<T>(*this);
            }
            return RefPtr<T>();
        }

        // Check if the object is still valid
        bool expired() const {
            return !refCount || *refCount == 0;
        }

        // Get the reference count
        size_t use_count() const {
            return refCount ? *refCount : 0;
        }
    };

} // namespace Sleak

#endif // _WEAK_PTR_H_