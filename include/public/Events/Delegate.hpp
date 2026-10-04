#ifndef _DELEGATE_H_
#define _DELEGATE_H_

#include <Utility/Container/List.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Sleak {
    /// Type-erased callable interface storable in a homogeneous handler list.
    /// @ingroup events
    class IDelegate {
        public:
            virtual ~IDelegate() = default;
            virtual void Execute() = 0;
            virtual std::string GetID() = 0;
        };

        // Standard delegate for non-event parameters
        /// Callable wrapper that stashes its arguments via SetArgs and invokes them later via Execute.
        /// @ingroup events
        template<typename... Args>
        class Delegate : public IDelegate {
        public:
            using FunctionType = std::function<void(Args...)>;

            Delegate(FunctionType func) : function(func) {}

            void SetArgs(Args... args) {
                // Use perfect forwarding to handle the arguments
                storedArgs = std::forward_as_tuple(args...);
            }

            void Execute() override {
                if (function) {
                    std::apply(function, storedArgs);
                }
            }

            std::string GetID() override { return ""; };

        private:
            FunctionType function;
            std::tuple<Args...> storedArgs;  // Store by reference using forward_as_tuple
        };

        // Specialized delegate for Event types
        /// Common base for EventDelegate<T> instantiations, so they can share a handler list.
        /// @ingroup events
        class EventDelegateBase : public IDelegate {
        public:
         EventDelegateBase() { GenerateID(); }
         virtual ~EventDelegateBase() = default;

         /// Assigns a fresh process-unique ID, used to identify this delegate
         /// for unregistration.
         std::string GenerateID() {
             static std::atomic<uint64_t> nextHandle{0};
             handle = nextHandle.fetch_add(1, std::memory_order_relaxed) + 1;
             id = std::to_string(handle);
             return id;
         }

         std::string GetID() override { return id; }
         /// Numeric form of GetID().
         uint64_t GetHandle() const { return handle; }

        private:
         friend class EventDispatcher;

         uint64_t handle = 0;
         std::string id;
         bool removed = false;
        };

        /// Delegate bound to a single EventT callback; holds a non-owning pointer
        /// to the event set just before Execute() runs.
        /// @ingroup events
        template<typename EventT>
        class EventDelegate : public EventDelegateBase {
        public:
            using FunctionType = std::function<void(const EventT&)>;

            EventDelegate(FunctionType func) : function(std::move(func)) {}

            void SetEvent(const EventT& event) {
                eventPtr = &event;
            }

            void Execute() override {
                if (function && eventPtr) {
                    function(*eventPtr);
                }
            }

            /// Calls the handler with event directly.
            void Invoke(const EventT& event) {
                if (function) function(event);
            }

        private:
            FunctionType function;
            const EventT* eventPtr =
                nullptr;  // Store a pointer to avoid copying
        };
        
        /// Fan-out list of Delegate<Args...> instances, all invoked together via Broadcast.
        /// @ingroup events
        template<typename... Args>
        class MulticastDelegate {
        public:
            void AddDelegate(std::shared_ptr<IDelegate> delegate) {
                delegates.push_back(delegate);
            }

            /// Sets args on and executes every registered delegate that matches Args.
            void Broadcast(Args... args) {
                for (auto& delegate : delegates) {
                    auto typedDelegate = std::dynamic_pointer_cast<Delegate<Args...>>(delegate);
                    if (typedDelegate) {
                        typedDelegate->SetArgs(std::forward<Args>(args)...);
                        typedDelegate->Execute();
                    }
                }
            }
    
        private:
            std::vector<std::shared_ptr<IDelegate>> delegates;
        };
    
        // Helper function for member functions
        /// Wraps a member function bound to obj into a shared Delegate.
        template<typename T, typename... Args>
        std::shared_ptr<Delegate<Args...>> CreateDelegate(T* obj, void (T::*func)(Args...)) {
            return std::make_shared<Delegate<Args...>>([obj, func](Args... args) {
                (obj->*func)(std::forward<Args>(args)...);
            });
        }
    };

#endif  // _DELEGATE_H_
