#ifndef _REF_PTR_
#define _REF_PTR_

#include <atomic>
#include <type_traits>
#include "SmartPointer.hpp"

namespace Sleak {

/// Reference count shared by every RefPtr aimed at one object.
///
/// Allocated separately from the object, and non-templated so that a
/// RefPtr<Base> converted from a RefPtr<Derived> shares the same count.
/// The counter is atomic, so copying and destroying RefPtrs across threads
/// is safe; the pointed-to object still needs its own synchronization.
/// @see RefPtr
/// @ingroup memory
struct SharedControlBlock {
    /// Number of RefPtr instances currently owning the object.
    std::atomic<size_t> refCount;
    SharedControlBlock() : refCount(1) {}
};

/// Atomic, intrusive-refcount smart pointer for engine resources. This is
/// the standard owning pointer in the engine, used instead of shared_ptr.
///
/// Construct one from a raw pointer and it takes ownership, deleting the
/// object when the last RefPtr to it goes away. Copies share the count;
/// moves transfer it. Use this wherever the public API asks for an owning
/// pointer, notably Material and the GPU buffer handles on MeshHandle.
///
/// The constructor is explicit, so ownership transfer is always visible at
/// the call site. Converting RefPtr<Derived> to RefPtr<Base> is allowed
/// and keeps the same control block. `use_count()` reports the current
/// reference count.
///
/// The reference count is thread-safe. The object it guards is not: two
/// threads calling into the same pointed-to object still need their own
/// synchronization. Note also that `get()` on the SmartPointer base throws
/// NullPointerException when the pointer is null, so test with
/// `IsValid()` or the bool conversion before dereferencing an optional
/// resource.
///
/// @code{.cpp}
/// // Take ownership of a freshly created material
/// auto* raw = new Sleak::Material();
/// raw->SetRoughness(0.35f);
/// Sleak::RefPtr<Sleak::Material> material(raw);
///
/// // Share it across several objects; each copy bumps the count
/// crate->AddComponent<Sleak::MaterialComponent>(material);
/// barrel->AddComponent<Sleak::MaterialComponent>(material);
///
/// if (material.IsValid()) {
///     material->SetMetallic(0.0f);      // operator-> reaches the object
/// }
/// @endcode
///
/// @see SharedControlBlock, SmartPointer, ObjectPtr, WeakPtr
/// @ingroup memory
template <typename T>
class RefPtr : public SmartPointer<T> {
    // Allow other RefPtr instantiations to access getControlBlock().
    template <typename U>
    friend class RefPtr;

protected:
    SharedControlBlock* controlBlock;  // Shared across all types.


    public:

    /// Drops one reference, deleting the object and control block at zero.
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
