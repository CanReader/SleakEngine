#ifndef _KEYBOARDEVENT_H_
#define _KEYBOARDEVENT_H_

#include <Events/Event.h>
#include <Input/KeyCodes.h>
#include <sstream>

#define if_key_press(key) if (!e.IsRepeat() && e.GetKeyCode() == Sleak::Input::KEY_CODE::key)
#define if_key_down(key) if (e.GetKeyCode() == Sleak::Input::KEY_CODE::key)

namespace Sleak {
    namespace Events {
        namespace Input {
            class ENGINE_API KeyEvent : public Event
            {
            public:
                KeyCode GetKeyCode() const { return m_KeyCode; }
                std::string GetKeyStr() const { return Sleak::Input::Key_toString(m_KeyCode); }
        
                EVENT_CLASS_CATEGORY(EventCategory::Keyboard)
            protected:
                KeyEvent(const KeyCode keycode)
                    : m_KeyCode(keycode) {}
        
                KeyCode m_KeyCode;
            };
            
            class ENGINE_API KeyPressedEvent : public KeyEvent
            {
            public:
                KeyPressedEvent(const KeyCode keycode, bool isRepeat = false)
                    : KeyEvent(keycode), m_IsRepeat(isRepeat) {}
        
                bool IsRepeat() const { return m_IsRepeat; }
        
                std::string ToString() const override
                {
                    std::stringstream ss;
                    const char* repeat = m_IsRepeat ? "Repeating" : "";
                    ss << "KeyPressedEvent: " << GetKeyStr() << " " << repeat;
                    return ss.str();
                }
        
                EVENT_CLASS_TYPE(KeyPressed)
            private:
                bool m_IsRepeat;
            };
        
            class ENGINE_API KeyReleasedEvent : public KeyEvent
            {
            public:
                KeyReleasedEvent(const KeyCode keycode)
                    : KeyEvent(keycode) {}
        
                std::string ToString() const override
                {
                    std::stringstream ss;
                    ss << "KeyReleasedEvent: " << GetKeyStr();
                    return ss.str();
                }
        
                EVENT_CLASS_TYPE(KeyReleased)
            };
        
            class ENGINE_API KeyTypedEvent : public KeyEvent
            {
            public:
                KeyTypedEvent(const KeyCode keycode)
                    : KeyEvent(keycode) {}
        
                std::string ToString() const override
                {
                    std::stringstream ss;
                    ss << "KeyTypedEvent: " << GetKeyStr();
                    return ss.str();
                }
        
                EVENT_CLASS_TYPE(KeyTyped)
            };
  }
 }
}

#endif