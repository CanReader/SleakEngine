#ifndef _INPUTACTION_HPP_
#define _INPUTACTION_HPP_

#include <vector>
#include <string>
#include <Input/InputManager.hpp>

namespace Sleak {
    namespace Input {
        /// A named input binding across keys, mouse buttons, and gamepad
        /// buttons/axes, queried as a single logical action.
        /// @ingroup input
        class InputAction {
            public:
                std::string name;
                std::vector<int> keyMappings;
                std::vector<int> mouseButtonMappings;
                std::vector<int> gamepadButtonMappings;
                std::vector<std::pair<int, float>> gamepadAxisMappings;

                InputAction(const std::string& name) : name(name) {}

                /// True if any bound key, button, or axis is currently active in inputManager.
                bool isPressed(const InputManager& inputManager) const;
            };
    }
}

#endif