#ifndef _EVENT_H_
#define _EVENT_H_

#include <Core/OSDef.hpp>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <Events/Delegate.h>


#define BIND_LAMBDA(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }
#define BIND_FUNC_0(Function, class_name) std::bind(&class_name::Function, this)
#define BIND_FUNC_1(Function, class_name) std::bind(&class_name::Function, this, std::placeholders::_1)
#define BIND_FUNC_2(Function, class_name) std::bind(&class_name::Function, this, std::placeholders::_2)
#define BIND_FUNC_3(Function, class_name) std::bind(&class_name::Function, this, std::placeholders::_3)

#define GET_DISPATCHER Sleak::EventDispatcher::GetInstance()

namespace Sleak {
    enum class EventType {
        Unknown = 0,
        WindowOpen, WindowClose, WindowResize, WindowFullscreen, WindowFocus, WindowLostFocus, WindowMoved,
        Tick, Update, Render,
        KeyPressed, KeyReleased, KeyTyped,
        MousePressed, MouseReleased, MouseMoved, MouseScrolled
    };

    enum class EventCategory {
        None = (1 << 0),
        Application = (1 << 1),
        Input = (1 << 2),
        Keyboard = (1 << 3),
        Mouse = (1 << 4),
        MouseButton = (1 << 5)
    };

    #define EVENT_CLASS_TYPE(type) \
    static EventType GetStaticType() \
    { \
        return EventType::type;\
    }\
    virtual EventType GetEventType() const override \
    {\
        return GetStaticType();\
    }\
    virtual const char* GetName() const override \
    {\
        return #type;\
    }

    #define EVENT_CLASS_CATEGORY(category)\
    virtual int GetCategoryFlags() const override \
     { return static_cast<int>(category); }

    class ENGINE_API Event {
    public:
        Event() {}

        virtual ~Event() = default;

        bool Handled = false;

        virtual EventType GetEventType() const = 0;
        virtual const char* GetName() const = 0;
        virtual int GetCategoryFlags() const = 0;
        virtual std::string ToString() const { return GetName(); }

        bool IsInCategory(EventCategory category)
        {
            return GetCategoryFlags() & (uint16_t)category;
        }
    };

    class ENGINE_API EventDispatcher {
        public:
            // Register a handler for any event type
            template<typename EventT>
            static std::string RegisterEventCallback(std::function<void(const EventT&)> callback) {
                EventType type = EventT::GetStaticType();
                
                auto delegate = std::make_shared<EventDelegate<EventT>>(callback);
                eventHandlers[type].push_back(delegate);
                
                return delegate->GetUUID();
            }
            
            // Register a member function handler
            template<typename T, typename EventT>
            static std::string RegisterEventHandler(T* instance, void (T::*memberFunction)(const EventT&)) {
                EventType type = EventT::GetStaticType();
                
                auto callback = [instance, memberFunction](const EventT& event) {
                    (instance->*memberFunction)(event);
                };
                
                auto delegate = std::make_shared<EventDelegate<EventT>>(callback);
                eventHandlers[type].push_back(delegate);

                return delegate->GetID();
            }

            static void UnregisterEvent(EventType type, std::string id) {
                for(auto it = eventHandlers[type].begin(); it != eventHandlers[type].end(); ++it) {
                    if((*it)->GetID() == id) {
                        eventHandlers[type].erase(it);
                        break;
                    }
                }
            }

            static void UnregisterEvents(EventType type) {
                for(auto it = eventHandlers[type].begin(); it != eventHandlers[type].end(); ++it) {
                        eventHandlers[type].erase(it);
                }
            }

            static void UnregisterAllEvents() {
                eventHandlers.clear();
            }
            
            // Dispatch an event to all registered handlers
            template<typename EventT>
            static void DispatchEvent(const EventT& event) {
                EventType type = event.GetEventType();
                
                if (eventHandlers.find(type) != eventHandlers.end()) {
                    for (auto& handler : eventHandlers[type]) {
                        // Try to cast to the right event delegate type
                        auto typedDelegate = std::dynamic_pointer_cast<EventDelegate<EventT>>(handler);
                        if (typedDelegate) {
                            typedDelegate->SetEvent(event);
                            typedDelegate->Execute();
                        }
                    }
                }
            }
            
            static void ClearEventHandlers() {
                eventHandlers.clear();
            }
            
        private:
            static inline std::unordered_map<EventType, std::vector<std::shared_ptr<IDelegate>>> eventHandlers;
        };
        
        
        // Helper function for easier event dispatching
        template<typename T, typename... Args>
        void DispatchEvent(Args&&... args) {
            T event(std::forward<Args>(args)...);
            EventDispatcher::DispatchEvent(event);
        }

    inline std::ostream& operator<<(std::ostream& os, const Event& e)
    {
        return os << e.ToString();
    }
}

#endif
