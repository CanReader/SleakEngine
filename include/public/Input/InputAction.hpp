#ifndef _INPUTACTION_HPP_
#define _INPUTACTION_HPP_

#include <Core/OSDef.hpp>
#include <Input/Gamepad.hpp>
#include <Input/KeyCodes.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Sleak {
namespace Input {

/// A named action bound to any number of keys, mouse buttons, gamepad
/// buttons and gamepad axes. It is down while any binding is down, and the
/// pressed/released edges are for the action as a whole, so holding W and
/// then also pressing Up does not fire a second press.
///
/// An axis binding is a direction and a threshold: a positive threshold
/// is active when the axis is at or above it, a negative one when it is at
/// or below it. Gamepad bindings match any connected controller.
/// @see InputManager
/// @ingroup input
class ENGINE_API InputAction {
   public:
    explicit InputAction(std::string name) : name(std::move(name)) {}

    std::string name;
    std::vector<KEY_CODE> keys;
    std::vector<MOUSE_CODE> mouseButtons;
    std::vector<GAMEPAD_BUTTON> gamepadButtons;
    std::vector<std::pair<GAMEPAD_AXIS, float>> gamepadAxes;

    InputAction& BindKey(KEY_CODE key);
    InputAction& BindMouseButton(MOUSE_CODE button);
    InputAction& BindGamepadButton(GAMEPAD_BUTTON button);
    InputAction& BindGamepadAxis(GAMEPAD_AXIS axis, float threshold = 0.5f);
    /// Removes every binding but keeps the action registered.
    void ClearBindings();

    /// True on the frame the action goes from inactive to active.
    bool IsPressed() const;
    /// True while any binding is active.
    bool IsHold() const;
    /// True on the frame the action goes from active to inactive.
    bool IsReleased() const;
    /// Strongest binding in [0, 1]: 1 for a held button, the magnitude for an
    /// axis past its threshold.
    float GetValue() const;
};

}  // namespace Input
}  // namespace Sleak

#endif
