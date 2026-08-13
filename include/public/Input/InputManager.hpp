#ifndef _INPUTMANAGER_HPP_
#define _INPUTMANAGER_HPP_

#include <InputEventListener.hpp>
#include <vector>

namespace Sleak {
    namespace Input {
        /// Static registry of InputEventListeners that raw input gets fanned out to.
        class InputManager {
            public:

                static void RegisterListener(InputEventListener* listener);
                static void UnregisterListener(InputEventListener* listener);
            private:
                static td::vector<InputEventListener*> EventListeners;
        };
    }
}

#endif