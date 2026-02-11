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

class ENGINE_API Window {
public:
  Window();
  Window(int width, int height, std::string name);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  bool InitializeWindow();
  void Update();
  void Close();

  inline bool ShouldClose() { 
    if(event.type == SDL_EVENT_QUIT)
      return true;
    else
      return false;
  }

  inline SDL_Window* GetSDLWindow() {return SDLWindow;}
  inline std::string GetWindowTitle() { return WindowName;}

  void SetFullScreen(bool bEnable);
  inline void ToggleFullScreen() { SetFullScreen(!bIsFullScreen); }
  inline bool GetIsFullScreen()  { return bIsFullScreen; }

  void SetImGuiReady(bool ready) { m_imguiReady = ready; }

  static int GetWidth() { return Width; }
  static int GetHeight() { return Height; }

private:
  bool bIsInitialized = false;
  bool bIsFullScreen = false;
  bool m_imguiReady = false;
  std::string WindowName;

  SDL_Window* SDLWindow;
  SDL_Event event;

  static int Width;
  static int Height;
};


}

#endif
