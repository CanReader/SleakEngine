#ifndef _KEYBOARD_HPP_
#define _KEYBOARD_HPP_

#include <bitset>
#include <functional>

#include <KeyCodes.h>

namespace Sleak {
    namespace Input {
class Keyboard {
    friend class Window;
    public:
    
        Keyboard() = default;
        ~Keyboard() = default;

        bool Initialize();
        void Shutdown();

        // Queries key states
        static bool IsKeyPressed(KEY_CODE key);
        static bool IsKeyHold(KEY_CODE key);
        static bool IsKeyReleased(KEY_CODE key);
    
    };
}
}

#endif