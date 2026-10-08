#ifndef _MOUSE_HPP_
#define _MOUSE_HPP_

#include <Core/OSDef.hpp>
#include <Input/KeyCodes.hpp>
#include <Math/Vector.hpp>

namespace Sleak {
namespace Input {

/// Per-frame mouse state: buttons, cursor position, and the motion and
/// wheel accumulated since the previous frame. Main thread only.
///
/// GetDelta() keeps working in relative mouse mode, where the position no
/// longer moves, so use it for camera look.
/// @see Keyboard, Gamepad, InputManager
/// @ingroup input
class ENGINE_API Mouse {
   public:
    /// True on the frame button transitions from up to down.
    static bool IsButtonPressed(MOUSE_CODE button);
    /// True while button is held down, including the initial press frame.
    static bool IsButtonHold(MOUSE_CODE button);
    /// True on the frame button transitions from down to up.
    static bool IsButtonReleased(MOUSE_CODE button);

    /// Cursor position in window pixels.
    static Math::Vector2D GetPosition();
    /// Motion since the previous frame, in pixels.
    static Math::Vector2D GetDelta();
    /// Wheel movement since the previous frame; positive y scrolls away from
    /// the user.
    static Math::Vector2D GetWheel();

    static void SetCursorVisible(bool visible);
};

}  // namespace Input
}  // namespace Sleak

#endif
