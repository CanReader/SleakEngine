#include "../../include/public/Core/Application.hpp"
#include "../../include/public/Core/CommandLine.hpp"
#include "../../include/private/Graphics/RendererFactory.hpp" 
#include "../../include/private/Graphics/RenderCommandQueue.hpp" 
#include <WindowHelper.hpp>
#include <Graphics/Renderer.hpp>
#include <Window.hpp>
#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include "Graphics/Vulkan/VulkanRenderer.hpp"
#include "Logger.hpp"
#include <Memory/ObjectPtr.h>

#include <Runtime/InternalGeometry.hpp>
#include <Camera/Camera.hpp>

namespace Sleak { class MeshBatch { public: static void Shutdown(); }; }
#include <Core/GameObject.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Random.hpp>
#include <Utility/Container/List.hpp>
#include <Core/ScopedTimer.h>
#include <Math/Matrix.hpp>
#include <UI/UI.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Graphics/Vertex.hpp>
#include "ECS/Components/MeshComponent.hpp"
#include "ECS/Components/TransformComponent.hpp"
#include "ECS/Components/FreeLookCameraController.hpp"
#include "ECS/Components/FirstPersonController.hpp"
#include <Runtime/Skybox.hpp>
#include <Runtime/Material.hpp>
#include <Debug/DebugOverlay.hpp>

using namespace Sleak;
using namespace Sleak::Math;

int width = 1200;
int height = 800;

namespace Sleak {
    Application* Application::Instance = nullptr;

    Application::Application(const char* Name) : 
    Application::Application(
        {Name, Arguments(0, nullptr)}
        ) 
    {}

    Application::Application(ApplicationDefaults Settings) : Specification(Settings) {
        if (Instance) {
            throw std::runtime_error("The Application is already running!");
        }
        Instance = this;

        // Read all settings from CommandLine (parsed in main before Application)
        {
            const std::string wStr = CommandLine::GetValue("-w");
            const std::string hStr = CommandLine::GetValue("-h");
            if (!wStr.empty()) try { width  = std::stoi(wStr); } catch (...) {}
            if (!hStr.empty()) try { height = std::stoi(hStr); } catch (...) {}
        }

        {
            std::string title = CommandLine::GetValue("-t");
            if (!title.empty()) {
                std::replace(title.begin(), title.end(), '_', ' ');
                Specification.Name = title;
            }
        }

        CoreWindow = new Window(width, height, Specification.Name);

        try {
            const std::string rendererArg = CommandLine::GetValue("-r");
            if (!rendererArg.empty()) {
                renderer = RenderEngine::RendererFactory::ParseArg(rendererArg, CoreWindow);
            } else {
                #ifdef PLATFORM_WIN
                renderer = RenderEngine::RendererFactory::CreateRenderer(
                    RenderEngine::RendererType::DirectX11, CoreWindow);
                #else
                renderer = RenderEngine::RendererFactory::CreateRenderer(
                    RenderEngine::RendererType::Vulkan, CoreWindow);
                #endif
            }
        }
        catch (std::exception& e) {
            SLEAK_ERROR(std::string(e.what()));
            renderer = RenderEngine::RendererFactory::CreateRenderer(
                RenderEngine::RendererType::Vulkan, CoreWindow);
        }

        EventDispatcher::RegisterEventHandler(this,&Application::OnKeyPressed);
        EventDispatcher::RegisterEventHandler(this, &Application::OnWindowResize);
        EventDispatcher::RegisterEventHandler(this, &Application::OnWindowFullScreen);
        
    }

    Application::~Application() {
        // Flush the GPU before tearing down any scene-owned resources (textures,
        // meshes, materials) — the renderer's descriptor sets still reference
        // samplers/image views owned by the game's scene objects. Without this
        // wait, Texture destructors call vkDestroySampler while the descriptor
        // set binding still has the sampler in use (VUID-vkDestroySampler-sampler-01082).
        if (renderer) renderer->WaitIdle();

        delete Game;
        Sleak::UI::ShutdownTextureCache();  // frees cached VkImage/memory pre-device-teardown
        Sleak::MeshBatch::Shutdown();
        delete m_benchmark;
        m_benchmark = nullptr;
        delete m_DebugOverlay;
        m_DebugOverlay = nullptr;

        if (renderer)
            renderer->Cleanup();

        delete renderer;
        delete CoreWindow;
        SLEAK_LOG("The application has been successfully closed, have a good day sir");
    }

    int Application::Run(GameBase* game) {
        Game = game;

        float lastTime = FrameTimer.Elapsed();

        if(CoreWindow && CoreWindow->InitializeWindow()) {

        if(renderer && renderer->Initialize()) {

            renderer->CreateImGUI();
            if (renderer->GetImGUIEnabled())
                CoreWindow->SetImGuiReady(true);

            m_DebugOverlay = new DebugOverlay();
            m_DebugOverlay->Initialize(renderer, game);

            m_benchmark = new Benchmark();
            m_benchmark->Initialize(renderer);
            {
                auto& cfg = m_DebugOverlay->GetConfig();
                cfg.ShowCameraPanel = false;
                cfg.ShowPerformancePanel = false;
            }

            auto context = renderer->GetContext();
            auto queue = RenderEngine::RenderCommandQueue::GetInstance();

            float lastTime = FrameTimer.Elapsed();
            float accumulator = 0.0f;
            const float fixedTimestep = 1.0f / 60.0f;  

            // Initialize and begin the game (and scene)
            if (Game) {
                if (!Game->Initialize()) {
                    SLEAK_FATAL("Game failed to initialize!");
                    return -1;
                }

                Game->Begin();

                // Apply CLI graphics settings
                {
                    if (CommandLine::HasFlag("--vsync"))    renderer->SetVSync(true);
                    if (CommandLine::HasFlag("--no-vsync")) renderer->SetVSync(false);

                    const std::string msaaStr = CommandLine::GetValue("-msaa");
                    if (!msaaStr.empty()) {
                        try { renderer->SetMSAASampleCount(
                            static_cast<uint32_t>(std::stoi(msaaStr))); }
                        catch (...) {}
                    }

                    if (CommandLine::HasFlag("--fullscreen")) CoreWindow->ToggleFullScreen();
                }

                // Auto-start benchmark if --bench / --benchmark was passed
                if ((CommandLine::HasFlag("--bench") || CommandLine::HasFlag("--benchmark"))
                    && m_benchmark)
                    m_benchmark->ToggleRecording();
            }

            while(!CoreWindow->ShouldClose()) {
                
                float currentTime = FrameTimer.Elapsed();
                DeltaTime = currentTime - lastTime;
                lastTime = currentTime;
                
                #if defined(_DEBUG) && defined(COUNT_FRAME)
                    ScopedTimer("Frame Timer");
                #endif

                CoreWindow->Update();

                // Apply any pending resize (deferred from event handler to avoid GPU hang)
                if (m_pendingResize) {
                    renderer->Resize(m_pendingResizeW, m_pendingResizeH);
                    width  = static_cast<int>(m_pendingResizeW);
                    height = static_cast<int>(m_pendingResizeH);
                    m_pendingResize = false;

                    // Update active camera's projection matrix for the new aspect ratio.
                    // Vulkan renderer ignores the width/height args to Resize() and uses
                    // the Vulkan surface caps, so the camera must be notified separately.
                    if (Game && Game->GetActiveScene()) {
                        if (auto* cam = Game->GetActiveScene()->GetActiveCamera())
                            cam->OnResize(m_pendingResizeW, m_pendingResizeH);
                    }
                }

                renderer->BeginRender();

                // Update active scene if present
                if (Game && Game->GetActiveScene()) {
                    auto* activeScene = Game->GetActiveScene();

                    // Fixed timestep updates (physics, etc.)
                    accumulator += DeltaTime;
                    while (accumulator >= fixedTimestep) {
                        activeScene->FixedUpdate(fixedTimestep);
                        accumulator -= fixedTimestep;
                    }

                    // Per-frame update
                    activeScene->Update(DeltaTime);

                    // Late update (after all updates, e.g. camera follow)
                    activeScene->LateUpdate(DeltaTime);
                }

                // Per-frame game logic
                if (Game)
                    Game->Loop(DeltaTime);
                if (m_DebugOverlay)
                    m_DebugOverlay->Render(DeltaTime);

                if (m_benchmark)
                    m_benchmark->Tick(DeltaTime);

                renderer->FlushPendingTransfers();

                if (queue && context)
                    queue->ExecuteCommands(context);

                renderer->EndRender();
            }
        } 
        else{
            SLEAK_FATAL("Unable to initialize graphics!");
            return -1;
        }
        
        }
        else {
            SLEAK_FATAL("App cannot run without any window!");
            return -2;
        }

        return 0;
    }

    void Application::OnWindowResize(const Sleak::Events::WindowResizeEvent& e) {
        // Store and defer — applying during event dispatch causes GPU hangs on rapid resize
        m_pendingResizeW = e.GetWidth();
        m_pendingResizeH = e.GetHeight();
        m_pendingResize  = true;
    }

    void Application::OnWindowFullScreen(const Sleak::Events::WindowFullScreen& e) {
        int w = Window::GetWidth();
        int h = Window::GetHeight();
        if (w > 0 && h > 0) {
            m_pendingResizeW = static_cast<uint32_t>(w);
            m_pendingResizeH = static_cast<uint32_t>(h);
            m_pendingResize  = true;
        }
    }

    void Application::onMouseClick(const Sleak::Events::Input::MouseButtonPressedEvent& e) {
        SLEAK_INFO(e.ToString());
    }

    void Application::onMouseMove(const Sleak::Events::Input::MouseMovedEvent& e) {
    }

    void Application::OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e) {

        switch(e.GetKeyCode())
        {
            case Input::KEY_CODE::KEY__ESCAPE:
              // Let the game handle ESC (e.g. return to menu)
              // Only close window if no game is running
              if (!Game)
                  GetWindow().Close();
            break;

            case Input::KEY_CODE::KEY__F1:
            break;

            case Input::KEY_CODE::KEY__F3:
            break;

            case Input::KEY_CODE::KEY__F4:
            break;

            case Input::KEY_CODE::KEY__F9:
            {
                if (Game && Game->GetActiveScene()) {
                    Camera* cam = Game->GetActiveScene()->GetActiveCamera();
                    if (cam) {
                        auto* fpc = cam->GetComponent<FirstPersonController>();
                        if (fpc) {
                            fpc->SetEnabled(!fpc->IsEnabled());
                        } else {
                            auto* ctrl = cam->GetComponent<FreeLookCameraController>();
                            if (ctrl)
                                ctrl->SetEnabled(!ctrl->IsEnabled());
                        }
                    }
                }
            }
            break;

            case Input::KEY_CODE::KEY__F11:
              CoreWindow->ToggleFullScreen();
            break;

            case Input::KEY_CODE::KEY__F12:
              if (m_benchmark)
                  m_benchmark->ToggleRecording();
            break;

        }

    }

    Window& Application::GetWindow() {
        return *CoreWindow;
    }

    void Application::CloseApplication() {
        CoreWindow->Close();
    }

    void Application::WaitGPUIdle() {
        if (renderer) renderer->WaitIdle();
    }

    void Application::SetCursorVisible(bool visible) {
        if (visible)
            SDL_ShowCursor();
        else
            SDL_HideCursor();
    }

    void Application::SetMouseRelativeMode(bool enabled) {
        if (CoreWindow)
            CoreWindow->SetRelativeMouseMode(enabled);
    }

    int Application::GetFPS() const { return renderer->GetFrameRate(); }
    float Application::GetFrameTime() const { return renderer->GetFrameTime(); }
    int Application::GetVertices() const { return renderer->GetVertices(); }
    int Application::GetTriangles() const { return renderer->GetTriangles(); }
    size_t Application::GetGPUMemoryUsed() const { return renderer->GetGPUMemoryUsed(); }
    size_t Application::GetGPUMemoryBudget() const { return renderer->GetGPUMemoryBudget(); }
    const char* Application::GetRendererTypeStr() const { return renderer->GetTypeStr(); }

    void Application::GetRendererTypeColor(float& r, float& g, float& b) const {
        switch (renderer->GetType()) {
            case RenderEngine::RendererType::DirectX12: r = 0.0f; g = 0.5f; b = 1.0f; break;
            case RenderEngine::RendererType::DirectX11: r = 0.2f; g = 0.6f; b = 0.8f; break;
            case RenderEngine::RendererType::Vulkan:    r = 0.8f; g = 0.2f; b = 0.0f; break;
            case RenderEngine::RendererType::OpenGL:    r = 0.0f; g = 0.8f; b = 0.2f; break;
            default:                                    r = 0.5f; g = 0.5f; b = 0.5f; break;
        }
    }

    uint32_t Application::GetMSAASampleCount() const { return renderer->GetMSAASampleCount(); }
    uint32_t Application::GetMaxMSAASampleCount() const { return renderer->GetMaxMSAASampleCount(); }
    void Application::SetMSAASampleCount(uint32_t samples) { renderer->SetMSAASampleCount(samples); }

    bool Application::GetVSync() const { return renderer->GetVSync(); }
    void Application::SetVSync(bool enabled) { renderer->SetVSync(enabled); }

    bool  Application::IsSSAOEnabled()  const { return renderer->IsSSAOEnabled(); }
    void  Application::SetSSAOEnabled(bool e)  { renderer->SetSSAOEnabled(e); }
    float Application::GetSSAORadius() const   { return renderer->GetSSAORadius(); }
    void  Application::SetSSAORadius(float r)  { renderer->SetSSAORadius(r); }
    float Application::GetSSAOBias()   const   { return renderer->GetSSAOBias(); }
    void  Application::SetSSAOBias(float b)    { renderer->SetSSAOBias(b); }
    float Application::GetSSAOPower()  const   { return renderer->GetSSAOPower(); }
    void  Application::SetSSAOPower(float p)   { renderer->SetSSAOPower(p); }

    bool  Application::IsIBLEnabled()  const   { return renderer->IsIBLEnabled(); }
    void  Application::SetIBLEnabled(bool e)   { renderer->SetIBLEnabled(e); }
    float Application::GetIBLIntensity() const { return renderer->GetIBLIntensity(); }
    void  Application::SetIBLIntensity(float i){ renderer->SetIBLIntensity(i); }

    bool  Application::IsSSREnabled()  const   { return renderer->IsSSREnabled(); }
    void  Application::SetSSREnabled(bool e)   { renderer->SetSSREnabled(e); }

    bool  Application::IsTAAEnabled()  const   { return renderer->IsTAAEnabled(); }
    void  Application::SetTAAEnabled(bool e)   { renderer->SetTAAEnabled(e); }

    bool  Application::IsBloomEnabled() const  { return renderer->IsBloomEnabled(); }
    void  Application::SetBloomEnabled(bool e) { renderer->SetBloomEnabled(e); }

    void Application::ApplyGraphicsConfig(const GraphicsConfig& cfg) {
        m_graphicsConfig = cfg;
        renderer->SetSSAOEnabled(cfg.ssaoEnabled);
        renderer->SetSSAORadius(cfg.ssaoRadius);
        renderer->SetSSAOBias(cfg.ssaoBias);
        renderer->SetSSAOPower(cfg.ssaoPower);
        renderer->SetSSREnabled(cfg.ssrEnabled);
        renderer->SetBloomEnabled(cfg.bloomEnabled);
        renderer->SetIBLEnabled(cfg.iblEnabled);
        renderer->SetIBLIntensity(cfg.iblIntensity);
        renderer->SetTAAEnabled(cfg.taaEnabled);
        renderer->SetShadowMapResolution(cfg.shadowMapResolution);
        renderer->SetMSAASampleCount(cfg.msaaSamples);
    }

    const GraphicsConfig& Application::GetGraphicsConfig() const {
        return m_graphicsConfig;
    }

}
