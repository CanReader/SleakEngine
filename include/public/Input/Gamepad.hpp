#ifndef _GAMEPAD_HPP_
#define _GAMEPAD_HPP_

#include <Core/OSDef.hpp>
#include <string>

namespace Sleak {
namespace Input {

/// Gamepad buttons in a standard layout, numbered to match SDL3's
/// SDL_GamepadButton. Face buttons are positional, so SOUTH is Xbox A and
/// PlayStation Cross.
/// @ingroup input
enum class GAMEPAD_BUTTON {
    SOUTH = 0,
    EAST,
    WEST,
    NORTH,
    BACK,
    GUIDE,
    START,
    LEFT_STICK,
    RIGHT_STICK,
    LEFT_SHOULDER,
    RIGHT_SHOULDER,
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    MISC1,
    RIGHT_PADDLE1,
    LEFT_PADDLE1,
    RIGHT_PADDLE2,
    LEFT_PADDLE2,
    TOUCHPAD,
    MISC2,
    MISC3,
    MISC4,
    MISC5,
    MISC6,
    COUNT
};

/// Gamepad axes, numbered to match SDL3's SDL_GamepadAxis. Sticks report
/// -1..1 with +y pointing down, triggers report 0..1.
/// @ingroup input
enum class GAMEPAD_AXIS {
    LEFT_X = 0,
    LEFT_Y,
    RIGHT_X,
    RIGHT_Y,
    LEFT_TRIGGER,
    RIGHT_TRIGGER,
    COUNT
};

/// Per-frame state for up to MaxGamepads connected controllers, filled from
/// SDL3's gamepad API. Hot plugging is handled: a controller connected
/// mid-session takes the lowest free index, and a disconnected one reads
/// as released with zeroed axes. Main thread only.
///
/// @code{.cpp}
/// using namespace Sleak::Input;
/// if (Gamepad::IsConnected()) {
///     float strafe = Gamepad::GetAxis(GAMEPAD_AXIS::LEFT_X);
///     if (Gamepad::IsButtonPressed(GAMEPAD_BUTTON::SOUTH)) Jump();
/// }
/// @endcode
/// @see Keyboard, Mouse, InputManager
/// @ingroup input
class ENGINE_API Gamepad {
   public:
    static constexpr int MaxGamepads = 4;

    static bool IsConnected(int index = 0);
    /// Number of controllers currently connected.
    static int GetConnectedCount();
    /// Controller name as reported by SDL, or an empty string.
    static std::string GetName(int index = 0);

    /// True on the frame button transitions from up to down.
    static bool IsButtonPressed(GAMEPAD_BUTTON button, int index = 0);
    /// True while button is held down, including the initial press frame.
    static bool IsButtonHold(GAMEPAD_BUTTON button, int index = 0);
    /// True on the frame button transitions from down to up.
    static bool IsButtonReleased(GAMEPAD_BUTTON button, int index = 0);

    /// Axis value with the deadzone removed and the rest rescaled to full
    /// range.
    static float GetAxis(GAMEPAD_AXIS axis, int index = 0);
    /// Axis value straight from the device, no deadzone.
    static float GetRawAxis(GAMEPAD_AXIS axis, int index = 0);

    /// Sets the per-axis deadzone, clamped to [0, 0.95]. Defaults to 0.15.
    static void SetDeadzone(float deadzone);
    static float GetDeadzone();
};

}  // namespace Input
}  // namespace Sleak

#endif
