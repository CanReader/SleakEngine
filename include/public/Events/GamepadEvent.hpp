#ifndef _GAMEPADEVENT_H_
#define _GAMEPADEVENT_H_

#include <Events/Event.hpp>

namespace Sleak {
namespace Events {
namespace Input {

/// Fired when a controller is connected and has been given a Gamepad index.
/// @ingroup events
class ENGINE_API GamepadConnectedEvent : public Event {
   public:
    explicit GamepadConnectedEvent(int index) : m_Index(index) {}
    /// Index to pass to the Input::Gamepad queries.
    int GetIndex() const { return m_Index; }
    EVENT_CLASS_TYPE(GamepadConnected)
    EVENT_CLASS_CATEGORY(EventCategory::Gamepad)

   private:
    int m_Index;
};

/// Fired after a controller is disconnected; its index is free again.
/// @ingroup events
class ENGINE_API GamepadDisconnectedEvent : public Event {
   public:
    explicit GamepadDisconnectedEvent(int index) : m_Index(index) {}
    int GetIndex() const { return m_Index; }
    EVENT_CLASS_TYPE(GamepadDisconnected)
    EVENT_CLASS_CATEGORY(EventCategory::Gamepad)

   private:
    int m_Index;
};

}  // namespace Input
}  // namespace Events
}  // namespace Sleak

#endif
