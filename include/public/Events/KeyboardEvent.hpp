#ifndef _KEYBOARDEVENT_H_
#define _KEYBOARDEVENT_H_

#include <Events/Event.hpp>
#include <Input/KeyCodes.hpp>
#include <sstream>
#include <utility>

#define if_key_press(key) if (!e.IsRepeat() && e.GetKeyCode() == Sleak::Input::KEY_CODE::key)
#define if_key_down(key) if (e.GetKeyCode() == Sleak::Input::KEY_CODE::key)

namespace Sleak {
    namespace Events {
        namespace Input {
            /// Common base for keyboard events; carries the key code involved.
            /// @ingroup events
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

            /// Fired on key-down; IsRepeat() distinguishes the initial press from OS auto-repeat.
            /// @ingroup events
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
        
            /// Fired on key-up.
            /// @ingroup events
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

            /// Fired for text input, after IME/layout composition rather than
            /// raw key-down. The window sends it while SDL text input is active
            /// (ImGui turns that on for its text fields), with the composed
            /// UTF-8 in GetText().
            /// @ingroup events
            class ENGINE_API KeyTypedEvent : public KeyEvent
            {
            public:
                KeyTypedEvent(const KeyCode keycode)
                    : KeyEvent(keycode) {}

                explicit KeyTypedEvent(std::string text)
                    : KeyEvent(KeyCode::KEY__UNKNOWN),
                      m_Text(std::move(text)) {}

                /// UTF-8 text produced by the keystroke; empty for the key code
                /// form.
                const std::string& GetText() const { return m_Text; }

                std::string ToString() const override
                {
                    std::stringstream ss;
                    ss << "KeyTypedEvent: "
                       << (m_Text.empty() ? GetKeyStr() : m_Text);
                    return ss.str();
                }

                EVENT_CLASS_TYPE(KeyTyped)

               private:
                std::string m_Text;
            };
  }
 }
}

#endif