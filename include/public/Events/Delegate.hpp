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
    class IDelegate {
        public:
            virtual ~IDelegate() = default;
            virtual void Execute() = 0; 
            virtual std::string GetID() = 0;
        };
    
        // Standard delegate for non-event parameters
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
        class EventDelegateBase : public IDelegate {
        public:
            virtual ~EventDelegateBase() = default;
            virtual std::string GetID() = 0;
        };
        
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
        
        template<typename... Args>
        class MulticastDelegate {
        public:
            void AddDelegate(std::shared_ptr<IDelegate> delegate) {
                delegates.push_back(delegate);
            }
    
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
        template<typename T, typename... Args>
        std::shared_ptr<Delegate<Args...>> CreateDelegate(T* obj, void (T::*func)(Args...)) {
            return std::make_shared<Delegate<Args...>>([obj, func](Args... args) {
                (obj->*func)(std::forward<Args>(args)...);
            });
        }
    };

#endif  // _DELEGATE_H_
