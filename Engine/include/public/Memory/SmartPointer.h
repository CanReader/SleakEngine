#ifndef _SMART_POINTER_H_
#define _SMART_POINTER_H_

#include <cstddef>
#include <Utility/Exception.hpp>
#include <iostream>

namespace Sleak {

    // Forward declaration of derived classes
    template <typename T>
    class ObjectPtr;

    template <typename T>
    class RefPtr;

    template <typename T>
    class WeakPtr;

    // Base SmartPointer class
    template <typename T>
    class SmartPointer {
    protected:
        T* ptr; // Raw pointer to the managed object

        // Protected constructor for derived classes
        explicit SmartPointer(T* p = nullptr) : ptr(p) {}

    public:
        // Virtual destructor for proper cleanup
        virtual ~SmartPointer() {}

        // Dereference operators
        T& operator*() const { return *ptr; }
        T* operator->() const { return ptr; }
        T* get() const 
        {
            if(ptr == nullptr)
                throw  Sleak::NullPointerException("Requested object is nullptr!");
                
            return ptr; 
        }

        virtual bool IsValid() const {
            return ptr != nullptr;
        }

        // Reset the pointer (to be overridden by derived classes)
        virtual void reset(T* newPtr = nullptr) = 0;

        // Check if the pointer is valid
        explicit operator bool() const { return IsValid(); }

        // Disallow copying (to be overridden by derived classes)
        SmartPointer(const SmartPointer&) = delete;
        SmartPointer& operator=(const SmartPointer&) = delete;
    };

} // namespace Sleak

#endif // _SMART_POINTER_H_