#ifndef _WINDOW_HPP_
#define _WINDOW_HPP_

#include <Core/OSDef.hpp>
#include <SDL3/SDL.h>
#include <string>
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_video.h"
#include <backends/imgui_impl_sdl3.h>

#define DEF_WIDTH 800
#define DEF_HEIGHT 600

namespace Sleak {

/// SDL window wrapper: owns the native window and pumps SDL events into
/// engine Events. Application owns exactly one.
class ENGINE_API Window {
public:
 /// Constructs a window with the default size and no title.
 Window();
 /// Constructs a window with the given size and title.
 Window(int width, int height, std::string name);
 /// Destroys the SDL window and shuts down SDL.
 ~Window();

 Window(const Window&) = delete;
 Window& operator=(const Window&) = delete;

 /// Creates the SDL window and picks the graphics API flag for the active
 /// renderer.
 bool InitializeWindow();
 /// Pumps the SDL event queue, dispatching engine events (resize, input, close,
 /// ...).
 void Update();
 /// Marks the window for close; actual teardown happens in the destructor.
 void Close();

 /// True once the window has received a close request.
 inline bool ShouldClose() { return bShouldClose; }

 /// Returns the underlying SDL window handle.
 inline SDL_Window* GetSDLWindow() { return SDLWindow; }
 /// Returns the window title.
 inline std::string GetWindowTitle() { return WindowName; }

 /// Enables or disables fullscreen mode.
 void SetFullScreen(bool bEnable);
 /// Flips between fullscreen and windowed mode.
 inline void ToggleFullScreen() { SetFullScreen(!bIsFullScreen); }
 /// True if the window is currently fullscreen.
 inline bool GetIsFullScreen() { return bIsFullScreen; }

 /// Marks whether ImGui is ready to receive SDL events.
 void SetImGuiReady(bool ready) { m_imguiReady = ready; }

 /// Enables or disables relative (locked, delta-only) mouse mode.
 void SetRelativeMouseMode(bool enabled);

 /// Returns the current window width in pixels.
 static int GetWidth() { return Width; }
 /// Returns the current window height in pixels.
 static int GetHeight() { return Height; }

private:
  bool bIsInitialized = false;
  bool bIsFullScreen = false;
  bool bShouldClose = false;
  bool m_imguiReady = false;
  std::string WindowName;

  SDL_Window* SDLWindow = nullptr;
  SDL_Event event;

  static int Width;
  static int Height;
};


}

#endif
