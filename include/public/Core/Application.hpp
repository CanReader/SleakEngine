#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <Core/OSDef.hpp>
#include <Core/GameBase.hpp>
#include <map>
#include <string>
#include <Events/ApplicationEvent.hpp>
#include <Events/InputEvent.hpp>
#include <Core/Timer.hpp>
#include <Core/GraphicsConfig.hpp>
#include <Debug/Benchmark.hpp>

namespace Sleak {

/// Parses raw argv into an index-and-flag lookup. Kept around on
/// ApplicationDefaults for the lifetime of the Application.
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

/// Construction settings for Application: project name plus parsed CLI args.
struct ENGINE_API ApplicationDefaults {
    std::string Name = "";
    Arguments CommandLineArgs;
};

class Window;
class DebugOverlay;
namespace RenderEngine { class Renderer; }

/// Owns the window, renderer, and game loop. One instance per process,
/// reachable globally through GetInstance().
class ENGINE_API Application {
   public:
    Application(const char* ProjectName);
    Application(ApplicationDefaults settings);
    Application& operator=(Application&&) = delete;
    Application& operator=(const Application&) = delete;
    ~Application();

    /// Drives the game loop until the window closes; returns the process exit code.
    int Run(GameBase* game);

    Window& GetWindow();
    void CloseApplication();
    /// Blocks until the GPU finishes all in-flight work. Call before tearing down scene resources.
    void WaitGPUIdle();

    void SetCursorVisible(bool visible);
    void SetMouseRelativeMode(bool enabled);

    /// Active renderer backend, or null before Run() initializes it.
    RenderEngine::Renderer* GetRenderer() { return renderer; }
    GameBase* GetGame() { return Game; }

    /// The one Application for this process, or null before construction.
    static Application* GetInstance() { return Instance; }

    // Public renderer stat accessors for Game
    /// Frames rendered in the last second.
    int GetFPS() const;
    /// Duration of the last frame, in seconds.
    float GetFrameTime() const;
    /// Vertices submitted in the last frame.
    int GetVertices() const;
    /// Triangles submitted in the last frame.
    int GetTriangles() const;
    /// Human-readable name of the active backend (e.g. "Vulkan").
    const char* GetRendererTypeStr() const;
    /// UI accent color associated with the active backend.
    void GetRendererTypeColor(float& r, float& g, float& b) const;
    size_t GetGPUMemoryUsed() const;
    size_t GetGPUMemoryBudget() const;
    uint32_t GetMSAASampleCount() const;
    uint32_t GetMaxMSAASampleCount() const;
    void SetMSAASampleCount(uint32_t samples);

    bool GetVSync() const;
    void SetVSync(bool enabled);

    // Screen-space ambient occlusion (deferred path only)
    bool  IsSSAOEnabled() const;
    void  SetSSAOEnabled(bool enabled);
    float GetSSAORadius() const;
    void  SetSSAORadius(float radius);
    float GetSSAOBias() const;
    void  SetSSAOBias(float bias);
    float GetSSAOPower() const;
    void  SetSSAOPower(float power);

    // Image-based lighting (deferred path only)
    bool  IsIBLEnabled() const;
    void  SetIBLEnabled(bool enabled);
    float GetIBLIntensity() const;
    void  SetIBLIntensity(float intensity);

    // Screen-space reflections (deferred path only)
    bool  IsSSREnabled() const;
    void  SetSSREnabled(bool enabled);

    // Temporal anti-aliasing (deferred path only)
    bool  IsTAAEnabled() const;
    void  SetTAAEnabled(bool enabled);

    // Bloom (deferred path only)
    bool  IsBloomEnabled() const;
    void  SetBloomEnabled(bool enabled);

    // Per-game graphics configuration / quality presets
    /// Pushes every field of cfg onto the active renderer in one call.
    void                  ApplyGraphicsConfig(const GraphicsConfig& cfg);
    /// Config last passed to ApplyGraphicsConfig().
    const GraphicsConfig& GetGraphicsConfig() const;

    // Active backend's feature capability mask (RenderEngine::GraphicsCaps)
    uint32_t              GetGraphicsCaps() const;

    Benchmark* GetBenchmark() { return m_benchmark; }

    /// Stashes the new size; the resize is applied at the start of the next frame.
    void OnWindowResize(const Sleak::Events::WindowResizeEvent& e);
    /// Treats entering/leaving fullscreen as a resize to the current window size.
    void OnWindowFullScreen(const Sleak::Events::WindowFullScreen& e);

    /// Handles engine-level hotkeys (F9 camera toggle, F11 fullscreen, F12 benchmark, Esc).
    void OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e);
    void onMouseMove(const Sleak::Events::Input::MouseMovedEvent& e);
    void onMouseClick(const Sleak::Events::Input::MouseButtonPressedEvent& e);

   private:
    ApplicationDefaults Specification;
    Window* CoreWindow;
    GameBase* Game;
    RenderEngine::Renderer* renderer;
    DebugOverlay* m_DebugOverlay = nullptr;
    Benchmark* m_benchmark = nullptr;
    GraphicsConfig m_graphicsConfig;
    float DeltaTime;

    Timer FrameTimer;

    // Deferred resize — set in the event handler, applied at start of next frame
    bool     m_pendingResize = false;
    uint32_t m_pendingResizeW = 0;
    uint32_t m_pendingResizeH = 0;

    static Application* Instance;
};

}  // namespace Sleak

#endif  // APPLICATION_HPP
