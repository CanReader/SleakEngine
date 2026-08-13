#ifndef _REF_PTR_
#define _REF_PTR_

#include <atomic>
#include <type_traits>
#include "SmartPointer.h"

namespace Sleak {

// A non-templated control block shared by all RefPtr instances.
struct SharedControlBlock {
    std::atomic<size_t> refCount;
    SharedControlBlock() : refCount(1) {}
};

template <typename T>
class RefPtr : public SmartPointer<T> {
    // Allow other RefPtr instantiations to access getControlBlock().
    template <typename U>
    friend class RefPtr;

protected:
    SharedControlBlock* controlBlock;  // Shared across all types.

    
    public:
    
    void release() {
        if (controlBlock && --controlBlock->refCount == 0) {
            delete this->ptr;
            delete controlBlock;
        }
    }
    // Constructor
    explicit RefPtr(T* p = nullptr)
        : SmartPointer<T>(p), controlBlock(p ? new SharedControlBlock() : nullptr) {}

    // Destructor
    ~RefPtr() { release(); }

    // Copy constructor
    RefPtr(const RefPtr& other)
        : SmartPointer<T>(other.ptr), controlBlock(other.controlBlock) {
        if (controlBlock) {
            controlBlock->refCount++;
        }
    }

    // Copy assignment
    RefPtr& operator=(const RefPtr& other) {
        if (this != &other) {
            release();
            this->ptr = other.ptr;
            controlBlock = other.controlBlock;
            if (controlBlock) {
                controlBlock->refCount++;
            }
        }
        return *this;
    }

    // Move constructor
    RefPtr(RefPtr&& other) noexcept
        : SmartPointer<T>(other.ptr), controlBlock(other.controlBlock) {
        other.ptr = nullptr;
        other.controlBlock = nullptr;
    }

    // Move assignment
    RefPtr& operator=(RefPtr&& other) noexcept {
        if (this != &other) {
            release();
            this->ptr = other.ptr;
            controlBlock = other.controlBlock;
            other.ptr = nullptr;
            other.controlBlock = nullptr;
        }
        return *this;
    }

    // Upcasting constructor: allow constructing RefPtr<Base> from RefPtr<Derived>
    template <typename U, typename = std::enable_if_t<std::is_base_of<T, U>::value>>
    RefPtr(const RefPtr<U>& other)
        : SmartPointer<T>(other.get()), controlBlock(other.getControlBlock()) {
        if (controlBlock) controlBlock->refCount++;
    }

    // Upcasting assignment operator
    template <typename U>
    RefPtr& operator=(const RefPtr<U>& other) {
        static_assert(std::is_base_of<T, U>::value, "T must be a base class of U");
        if (this != reinterpret_cast<const RefPtr*>(&other)) {
            release();
            this->ptr = other.get();
            controlBlock = other.getControlBlock();
            if (controlBlock) {
                controlBlock->refCount++;
            }
        }
        return *this;
    }

    // Assign nullptr
    RefPtr& operator=(std::nullptr_t) noexcept {
        release();
        this->ptr = nullptr;
        controlBlock = nullptr;
        return *this;
    }

    // Reset the pointer
    void reset(T* newPtr = nullptr) override {
        release();
        this->ptr = newPtr;
        controlBlock = newPtr ? new SharedControlBlock() : nullptr;
    }

    // Get the reference count
    size_t use_count() const {
        return controlBlock ? controlBlock->refCount.load() : 0;
    }

    // Provide access to the raw pointer.
    T* get() const { return this->ptr; }

protected:
    // Provide access to the shared control block.
    SharedControlBlock* getControlBlock() const { return controlBlock; }
};

}  // namespace Sleak

#endif // _REF_PTR_
