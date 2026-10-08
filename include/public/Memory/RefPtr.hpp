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
    /// Number of WeakPtr observers, plus one while any RefPtr is alive.
    std::atomic<size_t> weakCount;
    SharedControlBlock() : refCount(1), weakCount(1) {}

    /// Frees the block once the last strong and weak reference are gone.
    void ReleaseWeak() {
        if (weakCount.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
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
/// synchronization. `get()` returns nullptr for an empty RefPtr rather
/// than throwing like ObjectPtr does, so test with `IsValid()` or the bool
/// conversion before dereferencing an optional resource.
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
    template <typename U>
    friend class WeakPtr;

   protected:
    SharedControlBlock* controlBlock;  // Shared across all types.

    // Adopts a reference already taken on block, used by WeakPtr::lock().
    RefPtr(T* p, SharedControlBlock* block) noexcept
        : SmartPointer<T>(p), controlBlock(block) {}

   public:
    /// Drops this reference and leaves the RefPtr empty; the object is
    /// deleted when it was the last one.
    void release() {
        SharedControlBlock* block = controlBlock;
        T* object = this->ptr;
        this->ptr = nullptr;
        controlBlock = nullptr;
        if (block &&
            block->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete object;
            block->ReleaseWeak();
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
            RefPtr copy(other);
            release();
            this->ptr = copy.ptr;
            controlBlock = copy.controlBlock;
            copy.ptr = nullptr;
            copy.controlBlock = nullptr;
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
        RefPtr copy(other);
        release();
        this->ptr = copy.ptr;
        controlBlock = copy.controlBlock;
        copy.ptr = nullptr;
        copy.controlBlock = nullptr;
        return *this;
    }

    // Assign nullptr
    RefPtr& operator=(std::nullptr_t) noexcept {
        release();
        return *this;
    }

    // Reset the pointer
    void reset(T* newPtr = nullptr) override {
        if (newPtr && newPtr == this->ptr) return;
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
