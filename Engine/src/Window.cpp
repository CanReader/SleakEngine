#include "../../include/private/Window.hpp"

#include "../../include/private/Graphics/RendererFactory.hpp"
#include <cstdlib>
#include <cstring>
#include <Logger.hpp>
#include <Events/ApplicationEvent.h>
#include <Events/KeyboardEvent.h>
#include <Events/MouseEvent.h>


namespace Sleak {

int Window::Width = 0;
int Window::Height = 0;

Window::Window() : Window(DEF_WIDTH,DEF_HEIGHT,""){}

Window::Window(int width, int height, std::string name) 
    : WindowName(name),event() 
    {
      Width = width;
      Height = height;
    }

Window::~Window() {
  if (SDLWindow) {
    SDL_DestroyWindow(SDLWindow);
  }
  SDL_Quit();

  SLEAK_LOG("The window has been destroyed");
}

bool Window::InitializeWindow() {

  if(bIsInitialized)
    return false;

  auto type = RenderEngine::RendererFactory::GetRendererType();

  int GraphicsAPI = SDL_WINDOW_OPENGL;

  if(type == RenderEngine::RendererType::Vulkan)
    GraphicsAPI = SDL_WINDOW_VULKAN;

  if (!SDL_Init(SDL_INIT_VIDEO | GraphicsAPI)) {
    SLEAK_FATAL("Failed to initialize SDL: %{0}" , SDL_GetError());
    return false;
    }

  SDLWindow = SDL_CreateWindow(WindowName.c_str(), Width, Height, GraphicsAPI | SDL_WINDOW_RESIZABLE); 

  if (!SDLWindow) {
    SLEAK_FATAL("Failed to create window! {0} ", SDL_GetError()); 
    return false;
  }

  if(!SDL_ShowWindow(SDLWindow)) {
    SLEAK_FATAL("Failed to show window! {0} ", SDL_GetError()); 
    return false;

  }

    DispatchEvent<Sleak::Events::WindowOpenEvent>();

  SLEAK_INFO("Window has been initialized and created without any errors!");

  SLEAK_INFO("Current used operating system: {0}" , OS);
  #ifdef WINDOW_SYSTEM
  SLEAK_INFO("Current used window system: {0}" , WINDOW_SYSTEM);
  #else
  SLEAK_FATAL("No window system has been detected! are you sure you use a computer?" , WINDOW_SYSTEM);
  #endif

  return true;
}

void Window::Update() {
  while (SDL_PollEvent(&event)) {

    if (m_imguiReady)
      ImGui_ImplSDL3_ProcessEvent(&event);

    switch (event.type)
    {
      case SDL_EVENT_WINDOW_RESIZED:
      {
        uint16_t width = event.window.data1;
        uint16_t height = event.window.data2;

        Window::Width = width;
        Window::Height = height;

        DispatchEvent<Sleak::Events::WindowResizeEvent>(width,height);

        break;
      }
      case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
      {
        DispatchEvent<Sleak::Events::WindowFullScreen>(true);
        break;
      }
      case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      {
        DispatchEvent<Sleak::Events::WindowFullScreen>(false);
        break;
      }
      case SDL_EVENT_QUIT:
      {
        DispatchEvent<Sleak::Events::WindowCloseEvent>();
        EventDispatcher::UnregisterAllEvents();
        event.type = SDL_EVENT_QUIT;
        break;
      }

      case SDL_EVENT_KEY_DOWN:
      {
          KeyCode key = static_cast<KeyCode>(event.key.scancode);
          bool isRepeat = event.key.repeat;

          DispatchEvent<Sleak::Events::Input::KeyPressedEvent>(key,isRepeat);
          break;
      }

      case SDL_EVENT_KEY_UP:
      {
          KeyCode key = static_cast<KeyCode>(event.key.scancode);

          DispatchEvent<Sleak::Events::Input::KeyReleasedEvent>(key);
          break;
      }

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      {
          MouseCode button = (MouseCode)event.button.button;
          int x = event.button.x;
          int y = event.button.y;

          DispatchEvent<Sleak::Events::Input::MouseButtonPressedEvent>(button,x,y);
          break;
      }

      case SDL_EVENT_MOUSE_BUTTON_UP:
      {
        MouseCode button = (MouseCode)event.button.button;
        int x = event.button.x;
        int y = event.button.y;

        DispatchEvent<Sleak::Events::Input::MouseButtonReleasedEvent>(button,x,y);
        break;
      }

      case SDL_EVENT_MOUSE_MOTION:
      {
          int x = event.motion.x;
          int y = event.motion.y;
          int dx = event.motion.xrel;
          int dy = event.motion.yrel;

          DispatchEvent<Sleak::Events::Input::MouseMovedEvent>(x,y);
          break;
      }

      case SDL_EVENT_MOUSE_WHEEL:
      {
          int xOffset = event.wheel.x;
          int yOffset = event.wheel.y;

          DispatchEvent<Sleak::Events::Input::MouseScrolledEvent>(xOffset, yOffset);
          break;
      }
    }
  }
}

void Window::SetFullScreen(bool isFullscreen) {
  
  if(!SDL_SetWindowFullscreen(SDLWindow,isFullscreen))
  {
    SLEAK_INFO("Successfully changed window state: %s", isFullscreen ? "Fullscreen is enabled" : "Fullscreen is disabled");
    bIsFullScreen = isFullscreen;
  }
  else {
    SLEAK_ERROR("Failed to set full screen state! {}",SDL_GetError());
  }
}

void Window::Close() {
  event.type = SDL_EVENT_QUIT;
  SDL_Quit();
}

}