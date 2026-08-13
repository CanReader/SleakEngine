#ifndef _INPUTEVENTS_HPP_
#define _INPUTEVENTS_HPP_

namespace Sleak {
    namespace Input {

        enum class KeyboardEventType 
        {
            KeyPressed,
            KeyReleased,
            Hold
        };

        enum class MouseEventType 
        {
            ButtonPressed,
            ButtonReleased,
            Motion,
            Wheel,
        };

        enum class GamepadEventType {
            ButtonPressed,
            ButtonReleased,
            AxisMotion,
        };

        // Keyboard Events
        struct KeyboardEvent {
            int deviceID;
            KEY_CODE keyCode;
            KeyboardEventType Type;

            KeyboardEvent() : deviceID(0), keyCode(KEY_CODE::KEY__UNKNOWN){}
        };

        // Mouse Events
        struct MouseEvent {
            int deviceID;
            int mouseButton;
            int mouseX, mouseY;
            int mouseXRel, mouseYRel; // Relative motion
            int wheelX, wheelY; // Mouse wheel delta

            MouseEvent() : deviceID(0), mouseButton(0), mouseX(0), mouseY(0), mouseXRel(0), mouseYRel(0), wheelX(0), wheelY(0) {}
        };

        // Gamepad Events (if needed)
        struct GamepadEvent {
            int deviceID;
            int button; // SDL_CONTROLLER_BUTTON_*
            int axis;   // Axis index
            float axisValue; // -1.0f to 1.0f

            GamepadEvent() : deviceID(0), button(0), axis(0), axisValue(0.0f) {}
        };

    }
}

#endif