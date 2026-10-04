#ifndef _KEYBOARD_HPP_
#define _KEYBOARD_HPP_

#include <Core/OSDef.hpp>
#include <Input/KeyCodes.hpp>

namespace Sleak {
namespace Input {

/// Per-frame keyboard state, filled by the window from its event queue at
/// the start of every frame. Main thread only.
///
/// "Pressed" and "Released" are edges for the current frame and survive a
/// tap that goes down and up between two frames. OS key repeat is ignored.
///
/// @code{.cpp}
/// if (Sleak::Input::Keyboard::IsKeyPressed(KeyCode::KEY__E)) OpenDoor();
/// float forward = Sleak::Input::Keyboard::IsKeyHold(KeyCode::KEY__W) ? 1.0f :
/// 0.0f;
/// @endcode
/// @see Mouse, Gamepad, InputManager
/// @ingroup input
class ENGINE_API Keyboard {
   public:
    /// True on the frame key transitions from up to down.
    static bool IsKeyPressed(KEY_CODE key);
    /// True while key is held down, including the initial press frame.
    static bool IsKeyHold(KEY_CODE key);
    /// True on the frame key transitions from down to up.
    static bool IsKeyReleased(KEY_CODE key);
};

}  // namespace Input
}  // namespace Sleak

#endif
