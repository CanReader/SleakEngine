#ifndef _WEAK_PTR_H_
#define _WEAK_PTR_H_

#include <type_traits>
#include <utility>

#include "RefPtr.hpp"

namespace Sleak {

/// Non-owning observer of a RefPtr-managed object. It keeps the control
/// block alive but not the object, so lock() returns an empty RefPtr once
/// the last owner is gone instead of a dangling pointer.
/// @ingroup memory
template <typename T>
class WeakPtr {
    template <typename U>
    friend class WeakPtr;

   public:
    WeakPtr() noexcept : ptr(nullptr), controlBlock(nullptr) {}

    template <typename U,
              typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    WeakPtr(const RefPtr<U>& ref) noexcept
        : ptr(ref.ptr), controlBlock(ref.controlBlock) {
        Acquire();
    }

    WeakPtr(const WeakPtr& other) noexcept
        : ptr(other.ptr), controlBlock(other.controlBlock) {
        Acquire();
    }

    template <typename U,
              typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    WeakPtr(const WeakPtr<U>& other) noexcept
        : ptr(other.ptr), controlBlock(other.controlBlock) {
        Acquire();
    }

    WeakPtr(WeakPtr&& other) noexcept
        : ptr(other.ptr), controlBlock(other.controlBlock) {
        other.ptr = nullptr;
        other.controlBlock = nullptr;
    }

    ~WeakPtr() { reset(); }

    WeakPtr& operator=(const WeakPtr& other) noexcept {
        WeakPtr(other).swap(*this);
        return *this;
    }

    WeakPtr& operator=(WeakPtr&& other) noexcept {
        WeakPtr(std::move(other)).swap(*this);
        return *this;
    }

    template <typename U,
              typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    WeakPtr& operator=(const RefPtr<U>& ref) noexcept {
        WeakPtr(ref).swap(*this);
        return *this;
    }

    /// Stops observing, freeing the control block if nothing else uses it.
    void reset() noexcept {
        if (controlBlock) controlBlock->ReleaseWeak();
        ptr = nullptr;
        controlBlock = nullptr;
    }

    void swap(WeakPtr& other) noexcept {
        std::swap(ptr, other.ptr);
        std::swap(controlBlock, other.controlBlock);
    }

    /// Returns an owning RefPtr, or an empty one if the object is gone.
    RefPtr<T> lock() const noexcept {
        if (!controlBlock) return RefPtr<T>();
        size_t count = controlBlock->refCount.load(std::memory_order_relaxed);
        while (count != 0) {
            if (controlBlock->refCount.compare_exchange_weak(
                    count, count + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return RefPtr<T>(ptr, controlBlock);
            }
        }
        return RefPtr<T>();
    }

    /// True once every owning RefPtr has been released.
    bool expired() const noexcept { return use_count() == 0; }

    /// Number of RefPtr instances currently owning the object.
    size_t use_count() const noexcept {
        return controlBlock
                   ? controlBlock->refCount.load(std::memory_order_acquire)
                   : 0;
    }

   private:
    void Acquire() noexcept {
        if (controlBlock)
            controlBlock->weakCount.fetch_add(1, std::memory_order_relaxed);
    }

    T* ptr;
    SharedControlBlock* controlBlock;
};

}  // namespace Sleak

#endif  // _WEAK_PTR_H_
