#ifndef _INPUTACTION_HPP_
#define _INPUTACTION_HPP_

#include <vector>
#include <string>
#include <InputManager.h>

namespace Sleak {
    namespace Input {
        class InputAction {
            public:
                std::string name;
                std::vector<int> keyMappings; 
                std::vector<int> mouseButtonMappings;
                std::vector<int> gamepadButtonMappings;
                std::vector<std::pair<int, float>> gamepadAxisMappings; 
            
                InputAction(const std::string& name) : name(name) {}
            
                bool isPressed(const InputManager& inputManager) const;
            };
    }
}

#endif