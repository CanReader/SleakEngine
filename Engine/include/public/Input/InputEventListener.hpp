#ifndef _INPUTEVENTLISTENER_HPP_
#define _INPUTEVENTLISTENER_HPP_

#include <InputEvent.h>

namespace Sleak {
    namespace Input {
        class InputEventListener {

            public:
            virtual void onInputEvent(const KeyboardEvent& event) = 0;
            virtual void onKeyPress(const KeyboardEvent& event) = 0;
            virtual void onKeyRelease(const KeyboardEvent& event) = 0;
            
            virtual void onMousePress(const MouseEvent& event) = 0;
            virtual void onMouseRelease(const MouseEvent& event) = 0;
            virtual void onMouseMove(const MouseEvent& event) = 0;
            
            virtual void onGamepadButtonPress(const GamepadEvent& event) = 0;
            virtual void onGamepadButtonRelease(const GamepadEvent& event) = 0;
            virtual void onGamepadAxisMotion(const GamepadEvent& event) = 0;  

            virtual ~InputEventListener() = default;
        };
    }
}

#endif