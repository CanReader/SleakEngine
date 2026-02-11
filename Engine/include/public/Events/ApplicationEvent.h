#ifndef _APPLICATIONEVENT_H_
#define _APPLICATIONEVENT_H_

#include <Events/Event.h>
#include <sstream>

namespace Sleak {
    namespace Events {
        class ENGINE_API WindowResizeEvent : public Event {
            public:
                WindowResizeEvent(uint16_t Width, uint16_t Height) : width(Width), height(Height) {}
            
            inline uint16_t GetWidth() const { return width;}
            inline uint16_t GetHeight() const { return height;} 

            std::string ToString() const override {
                std::stringstream ss;
                ss << "WindowResizeEvent: Width: " << width << ", Height: " << height << "\n";
                return ss.str();
            }

            EVENT_CLASS_TYPE(WindowResize)
            EVENT_CLASS_CATEGORY(EventCategory::Application)

            private:
                uint16_t width, height;
        };

        class ENGINE_API WindowOpenEvent : public Event {
            public:
                WindowOpenEvent() = default;

                EVENT_CLASS_TYPE(WindowOpen)
                EVENT_CLASS_CATEGORY(EventCategory::Application)
        };

        class ENGINE_API WindowFullScreen : public Event {
            public:
                WindowFullScreen(bool bIsFullScreen) : bIsFullScreen(bIsFullScreen) {};

                EVENT_CLASS_TYPE(WindowFullscreen)
                EVENT_CLASS_CATEGORY(EventCategory::Application)

                inline bool GetFullscreen() const { return bIsFullScreen;}

            private:
                bool bIsFullScreen;
        };

        class ENGINE_API WindowCloseEvent : public Event {
            public:
                WindowCloseEvent() = default;

                EVENT_CLASS_TYPE(WindowClose)
                EVENT_CLASS_CATEGORY(EventCategory::Application)
        };

        
        class ENGINE_API TickEvent : public Event {
            public:
                TickEvent() = default;

                EVENT_CLASS_TYPE(Tick)
                EVENT_CLASS_CATEGORY(EventCategory::Application)
        };

        class ENGINE_API UpdateEvent : public Event {
            public:
                UpdateEvent() = default;

                EVENT_CLASS_TYPE(Update)
                EVENT_CLASS_CATEGORY(EventCategory::Application)
        };

        class ENGINE_API RenderEvent : public Event {
            public:
                RenderEvent() = default;

                EVENT_CLASS_TYPE(Render)
                EVENT_CLASS_CATEGORY(EventCategory::Application)
        };

    }
}

#endif