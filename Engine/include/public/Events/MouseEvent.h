#ifndef _MOUSEEVENT_H_
#define _MOUSEEVENT_H_

#include <Events/Event.h>
#include <sstream>
#include <Input/KeyCodes.h>

namespace Sleak {
    namespace Events {
        namespace Input {
            class ENGINE_API MouseMovedEvent : public Event {
                public:
                MouseMovedEvent(const int x, const int y) : mouseX(x), mouseY(y) {}
                
                int GetX() const { return mouseX; }
                int GetY() const { return mouseY; }
                
                std::string ToString() const override
		        {
                    std::stringstream ss;
                    ss << GetName() << " (" << mouseX << ", " << mouseY << ")";
                    return ss.str();
		        }
                
                
		        EVENT_CLASS_TYPE(MouseMoved)
		        EVENT_CLASS_CATEGORY(EventCategory::Mouse)
                private:
                    int mouseX, mouseY;
            };
            
            class MouseScrolledEvent : public Event
            {
                public:
                MouseScrolledEvent(const float xOffset, const float yOffset)
                : m_XOffset(xOffset), m_YOffset(yOffset) {}
                
                float GetXOffset() const { return m_XOffset; }
                float GetYOffset() const { return m_YOffset; }

                std::string ToString() const override
                {
                    std::stringstream ss;
                    ss << GetName() << " scroll amount (" << GetXOffset() << ", " << GetYOffset() << ")";
                    return ss.str();
                }
                
                EVENT_CLASS_TYPE(MouseScrolled)
                EVENT_CLASS_CATEGORY(EventCategory::Mouse)
                private:
                float m_XOffset, m_YOffset;
            };
            
            class MouseButtonEvent : public Event
            {
                public:
                    MouseCode GetMouseButton() const { return m_Button; }
                    std::string GetButtonStr() const { return Sleak::Input::Button_toString(m_Button); }

                    std::string ToString() const override
                {
                    std::stringstream ss;
                    ss << GetName() << " " << GetButtonStr() 
                       << " position (" << mouseX << ", " << mouseY << ")";
                    return ss.str();
                }

                EVENT_CLASS_CATEGORY(EventCategory::MouseButton)
                protected:
                    MouseButtonEvent(const MouseCode button, int x, int y) 
                    : m_Button(button), mouseX(x), mouseY(y) {}
                
                MouseCode m_Button;
                int mouseX, mouseY;
            };
            
            class MouseButtonPressedEvent : public MouseButtonEvent
            {
                public:
                MouseButtonPressedEvent(const MouseCode button, int x, int y)
                : MouseButtonEvent(button, x, y) {}
                
                EVENT_CLASS_TYPE(MousePressed)
            };
            
            class MouseButtonReleasedEvent : public MouseButtonEvent
            {
                public:
                MouseButtonReleasedEvent(const MouseCode button, int x, int y)
                : MouseButtonEvent(button,x,y) {}
        
		        EVENT_CLASS_TYPE(MouseReleased)
	        };
  }
 }
}

#endif