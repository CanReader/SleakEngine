#ifndef _INPUTMANAGER_HPP_
#define _INPUTMANAGER_HPP_

#include <Core/OSDef.hpp>
#include <Input/InputAction.hpp>
#include <Input/InputEventListener.hpp>
#include <string_view>

namespace Sleak {
namespace Input {

/// Owns the named action map and the raw input listeners. Keyboard, Mouse
/// and Gamepad answer device questions; this answers "is Jump pressed".
/// Main thread only.
///
/// @code{.cpp}
/// using namespace Sleak::Input;
/// InputManager::AddAction("Jump")
///     .BindKey(KEY_CODE::KEY__SPACE)
///     .BindGamepadButton(GAMEPAD_BUTTON::SOUTH);
///
/// if (InputManager::IsActionPressed("Jump")) Jump();
/// @endcode
/// @see InputAction, Keyboard, Mouse, Gamepad
/// @ingroup input
class ENGINE_API InputManager {
   public:
    /// Creates the action, or returns the existing one with that name.
    /// The reference stays valid until the action is removed.
    static InputAction& AddAction(std::string_view name);
    /// Returns nullptr if no action has that name.
    static InputAction* GetAction(std::string_view name);
    static void RemoveAction(std::string_view name);
    static void ClearActions();

    /// False for an unknown action.
    static bool IsActionPressed(std::string_view name);
    static bool IsActionHold(std::string_view name);
    static bool IsActionReleased(std::string_view name);
    /// 0 for an unknown action.
    static float GetActionValue(std::string_view name);

    /// Adds a listener that receives every raw key, mouse and gamepad event.
    static void RegisterListener(InputEventListener* listener);
    /// Safe to call from inside a listener callback.
    static void UnregisterListener(InputEventListener* listener);
};

}  // namespace Input
}  // namespace Sleak

#endif
