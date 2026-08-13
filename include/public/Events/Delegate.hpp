#ifndef _DELEGATE_H_
#define _DELEGATE_H_

#include <functional>
#include <vector>
#include <memory>
#include <tuple>
#include <utility>

#include <random>
#include <sstream>
#include <Utility/Container/List.hpp>

namespace Sleak {
    /// Type-erased callable interface storable in a homogeneous handler list.
    class IDelegate {
        public:
            virtual ~IDelegate() = default;
            virtual void Execute() = 0;
            virtual std::string GetID() = 0;
        };

        // Standard delegate for non-event parameters
        /// Callable wrapper that stashes its arguments via SetArgs and invokes them later via Execute.
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
        class EventDelegateBase : public IDelegate {
        public:
            virtual ~EventDelegateBase() = default;
            virtual std::string GetID() = 0;
        };

        /// Delegate bound to a single EventT callback; holds a non-owning pointer
        /// to the event set just before Execute() runs.
        template<typename EventT>
        class EventDelegate : public EventDelegateBase {
        public:
            using FunctionType = std::function<void(const EventT&)>;

            EventDelegate(FunctionType func) : function(func) {
                GenerateID();
            }

            void SetEvent(const EventT& event) {
                eventPtr = &event;
            }

            void Execute() override {
                if (function && eventPtr) {
                    function(*eventPtr);
                }
            }

            /// Assigns a fresh random 32-character ID, used to identify this delegate for unregistration.
            std::string GenerateID() {
                uuid = "";
                std::string str =
                    "0123456789!^#%&=*?+-_/"
                    "[]{}()"
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
                std::random_device dev;
                std::mt19937 rand(dev());
                std::uniform_int_distribution<> dist(0,str.size() - 1);

                for(int i = 0; i < 32; i++)
                    uuid += str[dist(rand)];

                return uuid;
            }

            std::string GetID() override { return uuid; }
    
        private:
            FunctionType function;
            const EventT* eventPtr = nullptr;  // Store a pointer to avoid copying
            std::string uuid;
        };
        
        /// Fan-out list of Delegate<Args...> instances, all invoked together via Broadcast.
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
