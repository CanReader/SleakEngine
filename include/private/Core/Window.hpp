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
  Window();
  Window(int width, int height, std::string name);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  /// Creates the SDL window and picks the graphics API flag for the active renderer.
  bool InitializeWindow();
  /// Pumps the SDL event queue, dispatching engine events (resize, input, close, ...).
  void Update();
  /// Marks the window for close; actual teardown happens in the destructor.
  void Close();

  inline bool ShouldClose() { return bShouldClose; }

  inline SDL_Window* GetSDLWindow() {return SDLWindow;}
  inline std::string GetWindowTitle() { return WindowName;}

  void SetFullScreen(bool bEnable);
  inline void ToggleFullScreen() { SetFullScreen(!bIsFullScreen); }
  inline bool GetIsFullScreen()  { return bIsFullScreen; }

  void SetImGuiReady(bool ready) { m_imguiReady = ready; }

  void SetRelativeMouseMode(bool enabled);

  static int GetWidth() { return Width; }
  static int GetHeight() { return Height; }

private:
  bool bIsInitialized = false;
  bool bIsFullScreen = false;
  bool bShouldClose = false;
  bool m_imguiReady = false;
  std::string WindowName;

  SDL_Window* SDLWindow;
  SDL_Event event;

  static int Width;
  static int Height;
};


}

#endif
