#ifndef _INPUT_BACKEND_HPP_
#define _INPUT_BACKEND_HPP_

#include <SDL3/SDL_events.h>

namespace Sleak {
namespace Input {
namespace Backend {

/// Clears last frame's edges and deltas; call once before pumping events.
void BeginFrame();
/// Folds one SDL event into the polling state and raw listeners.
/// Returns the gamepad index for a connect/disconnect event, else -1.
int ProcessEvent(const SDL_Event& event);
/// Releases every key and button, e.g. when the window loses focus.
void ReleaseAll();
/// Closes all open gamepads; call before SDL_Quit.
void Shutdown();

}  // namespace Backend
}  // namespace Input
}  // namespace Sleak

#endif
