#ifndef _EVENT_H_
#define _EVENT_H_

#include <Core/OSDef.hpp>
#include <Events/Delegate.hpp>
#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#define BIND_LAMBDA(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }
#define BIND_FUNC_0(Function, class_name) std::bind(&class_name::Function, this)
#define BIND_FUNC_1(Function, class_name) std::bind(&class_name::Function, this, std::placeholders::_1)
#define BIND_FUNC_2(Function, class_name)                         \
    std::bind(&class_name::Function, this, std::placeholders::_1, \
              std::placeholders::_2)
#define BIND_FUNC_3(Function, class_name)                         \
    std::bind(&class_name::Function, this, std::placeholders::_1, \
              std::placeholders::_2, std::placeholders::_3)

#define GET_DISPATCHER Sleak::EventDispatcher::GetInstance()

namespace Sleak {
    /// Concrete kind of Event; each Event subclass reports one of these via GetEventType().
    /// @ingroup events
    enum class EventType {
        Unknown = 0,
        WindowOpen, WindowClose, WindowResize, WindowFullscreen, WindowFocus, WindowLostFocus, WindowMoved,
        Tick, Update, Render,
        KeyPressed, KeyReleased, KeyTyped,
        MousePressed, MouseReleased, MouseMoved, MouseScrolled
    };

    /// Bitmask groups an Event can belong to, queried via Event::IsInCategory().
    /// @ingroup events
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

    /// Base for all engine events; carries type/category identity and the
    /// Handled flag a handler can set to stop further propagation.
    /// @ingroup events
    class ENGINE_API Event {
    public:
        Event() {}

        virtual ~Event() = default;

        /// Set from a handler to skip the handlers registered after it.
        mutable bool Handled = false;

        virtual EventType GetEventType() const = 0;
        virtual const char* GetName() const = 0;
        virtual int GetCategoryFlags() const = 0;
        virtual std::string ToString() const { return GetName(); }

        bool IsInCategory(EventCategory category)
        {
            return GetCategoryFlags() & (uint16_t)category;
        }
    };

    class EventSubscription;

    /// Static registry mapping EventType to subscribed handlers; dispatches
    /// events synchronously to every handler registered for its type.
    ///
    /// Everything here is static, so there is no dispatcher instance to
    /// pass around: subscribe from anywhere, and the window layer's
    /// keyboard, mouse, and window events reach you. RegisterEventHandler()
    /// binds a member function and is the form you will use most;
    /// RegisterEventCallback() takes a std::function for lambdas and free
    /// functions.
    ///
    /// Both return a string id. Keep it and pass it to UnregisterEvent()
    /// when the subscriber goes away, typically in a scene's OnDeactivate()
    /// or destructor. Handlers outlive the objects they were bound to
    /// otherwise, and the next dispatch calls into freed memory. Subscribe()
    /// does the same but returns an EventSubscription that unregisters
    /// itself when destroyed.
    ///
    /// Dispatch is synchronous and runs in registration order on the
    /// calling thread, stopping early once a handler sets Event::Handled.
    /// A handler may register or unregister during dispatch: a handler
    /// removed mid-dispatch is not called again, and one added mid-dispatch
    /// first runs on the next event. Not thread-safe; use from the main
    /// thread only.
    ///
    /// @code{.cpp}
    /// class WorldScene : public Sleak::Scene {
    /// public:
    ///     void Begin() override {
    ///         m_keyId = Sleak::EventDispatcher::RegisterEventHandler(
    ///             this, &WorldScene::OnKeyPressed);
    ///         Sleak::Scene::Begin();
    ///     }
    ///
    ///     void OnDeactivate() override {
    ///         if (!m_keyId.empty()) {
    ///             Sleak::EventDispatcher::UnregisterEvent(
    ///                 Sleak::EventType::KeyPressed, m_keyId);
    ///             m_keyId.clear();
    ///         }
    ///         Sleak::Scene::OnDeactivate();
    ///     }
    ///
    ///     void OnKeyPressed(
    ///         const Sleak::Events::Input::KeyPressedEvent& e) {
    ///         if_key_press(KEY__F) { ToggleFlashlight(); }
    ///     }
    ///
    /// private:
    ///     std::string m_keyId;
    /// };
    /// @endcode
    ///
    /// @see Event, EventType, EventSubscription,
    ///      Events::Input::KeyPressedEvent, Events::Input::MouseMovedEvent
    /// @ingroup events
    class ENGINE_API EventDispatcher {
       public:
        /// Registers a free-function/lambda callback for EventT, returning an
        /// ID for later unregistration.
        template <typename EventT>
        static std::string RegisterEventCallback(
            std::function<void(const EventT&)> callback) {
            auto delegate =
                std::make_shared<EventDelegate<EventT>>(std::move(callback));
            eventHandlers[EventT::GetStaticType()].push_back(delegate);
            return delegate->GetID();
        }

            /// Registers a member-function handler bound to instance, returning an ID for later unregistration.
            template <typename T, typename EventT>
            static std::string RegisterEventHandler(
                T* instance, void (T::*memberFunction)(const EventT&)) {
                auto callback = [instance, memberFunction](const EventT& event) {
                    (instance->*memberFunction)(event);
                };
                return RegisterEventCallback<EventT>(std::move(callback));
            }

            /// Like RegisterEventCallback(), but unregisters when the returned
            /// token is destroyed.
            template <typename EventT>
            [[nodiscard]] static EventSubscription Subscribe(
                std::function<void(const EventT&)> callback);

            /// Like RegisterEventHandler(), but unregisters when the returned
            /// token is destroyed.
            template <typename T, typename EventT>
            [[nodiscard]] static EventSubscription Subscribe(
                T* instance, void (T::*memberFunction)(const EventT&));

            /// Removes the single handler with matching id from type's handler list.
            static void UnregisterEvent(EventType type, const std::string& id) {
                uint64_t handle = 0;
                auto [end, ec] =
                    std::from_chars(id.data(), id.data() + id.size(), handle);
                if (ec != std::errc() || end != id.data() + id.size()) return;

                auto found = eventHandlers.find(type);
                if (found == eventHandlers.end()) return;

                auto& handlers = found->second;
                for (size_t i = 0; i < handlers.size(); ++i) {
                    if (handlers[i]->GetHandle() != handle ||
                        handlers[i]->removed)
                        continue;
                    if (dispatchDepth > 0) {
                        handlers[i]->removed = true;
                        needsCompaction = true;
                    } else {
                        handlers.erase(handlers.begin() + i);
                    }
                    return;
                }
            }

            /// Drops every handler registered for type.
            static void UnregisterEvents(EventType type) {
                auto found = eventHandlers.find(type);
                if (found == eventHandlers.end()) return;
                if (dispatchDepth > 0) {
                    MarkRemoved(found->second);
                } else {
                    found->second.clear();
                }
            }

            /// Drops every handler for every event type.
            static void UnregisterAllEvents() {
                if (dispatchDepth > 0) {
                    for (auto& [type, handlers] : eventHandlers)
                        MarkRemoved(handlers);
                } else {
                    eventHandlers.clear();
                }
            }

            /// Invokes every handler registered for event's type, in registration order.
            template<typename EventT>
            static void DispatchEvent(const EventT& event) {
                auto found = eventHandlers.find(event.GetEventType());
                if (found == eventHandlers.end()) return;

                auto& handlers = found->second;
                const size_t count = handlers.size();
                DispatchScope scope;
                for (size_t i = 0; i < count && !event.Handled; ++i) {
                    EventDelegateBase* handler = handlers[i].get();
                    if (handler->removed) continue;
                    if (auto* typed =
                            dynamic_cast<EventDelegate<EventT>*>(handler))
                        typed->Invoke(event);
                }
            }

            /// Drops every handler for every event type; equivalent to UnregisterAllEvents().
            static void ClearEventHandlers() { UnregisterAllEvents(); }

           private:
            using HandlerList = std::vector<std::shared_ptr<EventDelegateBase>>;

            // Removals during dispatch are deferred so indices and delegates
            // stay valid.
            struct DispatchScope {
                DispatchScope() { ++dispatchDepth; }
                ~DispatchScope() {
                    if (--dispatchDepth == 0 && needsCompaction) Compact();
                }
            };

            static void MarkRemoved(HandlerList& handlers) {
                for (auto& handler : handlers) handler->removed = true;
                needsCompaction = true;
            }

            static void Compact() {
                needsCompaction = false;
                for (auto& [type, handlers] : eventHandlers) {
                    std::erase_if(handlers,
                                  [](const auto& h) { return h->removed; });
                }
            }

            static inline std::unordered_map<EventType, HandlerList>
                eventHandlers;
            static inline int dispatchDepth = 0;
            static inline bool needsCompaction = false;
    };

    /// Move-only owner of one handler registration; unregisters it on
    /// destruction or Reset().
    /// @ingroup events
    class EventSubscription {
       public:
        EventSubscription() = default;
        EventSubscription(EventType type, std::string id)
            : m_type(type), m_id(std::move(id)) {}

        EventSubscription(EventSubscription&& other) noexcept
            : m_type(other.m_type), m_id(std::move(other.m_id)) {
            other.m_id.clear();
        }

        EventSubscription& operator=(EventSubscription&& other) noexcept {
            if (this != &other) {
                Reset();
                m_type = other.m_type;
                m_id = std::move(other.m_id);
                other.m_id.clear();
            }
            return *this;
        }

        EventSubscription(const EventSubscription&) = delete;
        EventSubscription& operator=(const EventSubscription&) = delete;

        ~EventSubscription() { Reset(); }

        /// Unregisters the handler now; safe to call more than once.
        void Reset() {
            if (m_id.empty()) return;
            EventDispatcher::UnregisterEvent(m_type, m_id);
            m_id.clear();
        }

        bool IsActive() const { return !m_id.empty(); }
        EventType GetType() const { return m_type; }
        const std::string& GetID() const { return m_id; }

       private:
        EventType m_type = EventType::Unknown;
        std::string m_id;
    };

    template <typename EventT>
    EventSubscription EventDispatcher::Subscribe(
        std::function<void(const EventT&)> callback) {
        return EventSubscription(
            EventT::GetStaticType(),
            RegisterEventCallback<EventT>(std::move(callback)));
    }

    template <typename T, typename EventT>
    EventSubscription EventDispatcher::Subscribe(
        T* instance, void (T::*memberFunction)(const EventT&)) {
        return EventSubscription(
            EventT::GetStaticType(),
            RegisterEventHandler(instance, memberFunction));
    }

        // Helper function for easier event dispatching
        /// Constructs a T from args and dispatches it through EventDispatcher.
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
