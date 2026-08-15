#ifndef _KEYBOARD_HPP_
#define _KEYBOARD_HPP_

#include <bitset>
#include <functional>

#include <Input/KeyCodes.hpp>

namespace Sleak {
    namespace Input {
/// Static keyboard state queried by key code; Window feeds it raw key events.
/// @ingroup input
class Keyboard {
    friend class Window;
    public:

        Keyboard() = default;
        ~Keyboard() = default;

        bool Initialize();
        void Shutdown();

        // Queries key states
        /// True on the frame key transitions from up to down.
        static bool IsKeyPressed(KEY_CODE key);
        /// True while key is held down, including the initial press frame.
        static bool IsKeyHold(KEY_CODE key);
        /// True on the frame key transitions from down to up.
        static bool IsKeyReleased(KEY_CODE key);

    };
}
}

#endif