#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <Core/OSDef.hpp>
#include <GameBase.hpp>
#include <map>
#include <string>
#include <Events/ApplicationEvent.h>
#include <Events/InputEvent.h>
#include <Core/Timer.hpp>

namespace Sleak {

struct ENGINE_API Arguments {
    Arguments(int argc, char** argv) : Size(argc), Args(argv) {
        for (int i = 1; i < Size; i++) {
            std::string arg = Args[i - 1];
            std::string argnext = Args[i];

            if (arg[0] == '-' && argnext[0] != '-') ArgMap[arg] = argnext;
        }
        
    }

    const char* operator[](int index) const {
        if (index < Size)
            return Args[index];
        else
            return "";
    }

    const std::string operator[](std::string key) const {

        for(auto& pair : ArgMap)
            if(pair.first == key)
                return pair.second;

        return "";
    }

    int Size = 0;
    char** Args = nullptr;
    std::map<std::string, std::string> ArgMap;
};

struct ENGINE_API ApplicationDefaults {
    std::string Name = "";
    Arguments CommandLineArgs;
};

class Window;
class DebugOverlay;
namespace RenderEngine { class Renderer; }

class ENGINE_API Application {
   public:
    Application(const char* ProjectName);
    Application(ApplicationDefaults settings);
    Application& operator=(Application&&) = delete;
    Application& operator=(const Application&) = delete;
    ~Application();

    int Run(GameBase* game);

    Window& GetWindow();

    RenderEngine::Renderer* GetRenderer() { return renderer; }

    static Application* GetInstance() { return Instance; }

    void OnWindowResize(const Sleak::Events::WindowResizeEvent& e);
    void OnWindowFullScreen(const Sleak::Events::WindowFullScreen& e);

    void OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e);
    void onMouseMove(const Sleak::Events::Input::MouseMovedEvent& e);
    void onMouseClick(const Sleak::Events::Input::MouseButtonPressedEvent& e);

   private:
    ApplicationDefaults Specification;
    Window* CoreWindow;
    GameBase* Game;
    RenderEngine::Renderer* renderer;
    DebugOverlay* m_DebugOverlay = nullptr;
    float DeltaTime;

    Timer FrameTimer;

    static Application* Instance;
};

}  // namespace Sleak

#endif  // APPLICATION_HPP
