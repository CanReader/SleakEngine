#include "../../include/public/Core/Application.hpp"

#include <Assets/InternalGeometry.hpp>
#include <Camera/Camera.hpp>
#include <Core/GameObject.hpp>
#include <Core/ScopedTimer.hpp>
#include <Core/Window.hpp>
#include <Core/WindowHelper.hpp>
#include <Debug/DebugOverlay.hpp>
#include <Debug/ShaderHotReload.hpp>
#include <Graphics/Common/ConstantBuffer.hpp>
#include <Graphics/Common/Renderer.hpp>
#include <Math/Matrix.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Random.hpp>
#include <Memory/ObjectPtr.hpp>
#include <Runtime/Material.hpp>
#include <Runtime/MeshBatch.hpp>
#include <Runtime/MeshData.hpp>
#include <Runtime/Skybox.hpp>
#include <UI/UI.hpp>
#include <Utility/Container/List.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/Common/RendererFactory.hpp"
#include "../../include/public/Core/CommandLine.hpp"
#include "Core/Logger.hpp"
#include "ECS/Components/FirstPersonController.hpp"
#include "ECS/Components/FreeLookCameraController.hpp"
#include "ECS/Components/MeshComponent.hpp"
#include "ECS/Components/TransformComponent.hpp"
#include "Graphics/Vulkan/VulkanRenderer.hpp"

#define DEFAULT_CAPTURE_FRAME 600
// Fixed-step rate, also the capture-mode clock rate
#define FIXED_STEPS_PER_SECOND 60.0f

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
            renderer->RegisterBenchmarkMetrics(*m_benchmark);
            m_benchmark->RegisterMetric("CPU_frame_ms",
                                        [this]() { return m_cpuFrameMs; });
            {
                auto& cfg = m_DebugOverlay->GetConfig();
                cfg.ShowCameraPanel = false;
                cfg.ShowPerformancePanel = false;
            }

            auto context = renderer->GetContext();
            auto queue = RenderEngine::RenderCommandQueue::GetInstance();

            float lastTime = FrameTimer.Elapsed();
            float accumulator = 0.0f;
            const float fixedTimestep = 1.0f / FIXED_STEPS_PER_SECOND;

            // Frame capture
            const std::string capturePath = CommandLine::GetValue("-capture");
            const bool captureMode = !capturePath.empty();
            uint64_t captureFrame = DEFAULT_CAPTURE_FRAME;
            const std::string frameArg =
                CommandLine::GetValue("-capture-frame");
            if (!frameArg.empty()) {
                try {
                    captureFrame = std::stoull(frameArg);
                } catch (...) {
                }
            }
            if (captureMode)
                SLEAK_INFO(
                    "Frame capture: frame {} -> {} (fixed 1/60 s step, "
                    "resizes ignored)",
                    captureFrame, capturePath);
            m_frameIndex = 0;
            bool captureArmed = false;

            Debug::ShaderHotReload::AddSourceDirectory(SLEAK_ENGINE_SHADER_DIR);

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
                DeltaTime =
                    captureMode ? fixedTimestep : currentTime - lastTime;
                m_engineTime = captureMode ? static_cast<float>(m_frameIndex) /
                                                 FIXED_STEPS_PER_SECOND
                                           : currentTime;
                lastTime = currentTime;
                
                #if defined(_DEBUG) && defined(COUNT_FRAME)
                    ScopedTimer("Frame Timer");
                #endif

                CoreWindow->Update();

                // Apply any pending resize (deferred from event handler to avoid GPU hang)
                if (m_pendingResize && !captureMode) {
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

                if (captureMode && !captureArmed &&
                    m_frameIndex >= captureFrame) {
                    captureArmed = true;
                    if (!renderer->RequestFrameCapture(capturePath)) {
                        SLEAK_ERROR("Frame capture unavailable on {}",
                                    renderer->GetTypeStr());
                        CoreWindow->Close();
                    }
                }

                Debug::ShaderHotReload::Poll();

                renderer->BeginRender();
                const auto cpuFrameStart = std::chrono::steady_clock::now();

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

                m_cpuFrameMs =
                    std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - cpuFrameStart)
                        .count();
                renderer->EndRender();

                if (captureArmed && !renderer->IsFrameCapturePending())
                    CoreWindow->Close();
                ++m_frameIndex;
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

    const bool Application::GetIsDeferredEnabled() const {
        return renderer->GetDeferredEnabled();
    }
    void Application::SetDeferredEnabled(bool enable) {
        renderer->SetDeferredEnabled(enable);
    }

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

    float Application::GetSharpenStrength() const {
        return renderer->GetSharpenStrength();
    }
    void Application::SetSharpenStrength(float s) {
        renderer->SetSharpenStrength(s);
    }

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
        renderer->SetSharpenStrength(cfg.sharpenStrength);
        renderer->SetShadowMapResolution(cfg.shadowMapResolution);
        renderer->SetMSAASampleCount(cfg.msaaSamples);
        renderer->SetTonemappingEnabled(cfg.tonemapEnabled);
        renderer->SetExposure(cfg.exposure);
        renderer->SetGamma(cfg.gamma);
        renderer->SetTonemapOperator(cfg.tonemapOperator);
        renderer->SetTonemapWhiteCurve(cfg.tonemapWhiteCurve);
        renderer->SetTonemapWhitePath(cfg.tonemapWhitePath);
        renderer->SetSaturation(cfg.saturation);
        renderer->SetOutputDither(cfg.filmGrainEnabled);
        renderer->SetBloomMode(cfg.bloomMode);
        renderer->SetBloomBlend(cfg.bloomBlend);
        renderer->SetGBufferAlbedoSRGB(cfg.gbufferAlbedoSRGB);
        renderer->SetPCSSEnabled(cfg.pcssEnabled);
    }

    bool Application::SetFrameUserData(const void* data, uint32_t size) {
        static_assert(MAX_FRAME_USER_DATA_BYTES ==
                      RenderEngine::FRAME_USER_DATA_MAX_BYTES);
        if (renderer && renderer->SetFrameUserData(data, size)) return true;
        static bool warned = false;
        if (!warned)
            SLEAK_ERROR("SetFrameUserData: {} bytes rejected (limit {})", size,
                        MAX_FRAME_USER_DATA_BYTES);
        warned = true;
        return false;
    }

    const GraphicsConfig& Application::GetGraphicsConfig() const {
        return m_graphicsConfig;
    }

    uint32_t Application::GetGraphicsCaps() const {
        return renderer->GetFeatureCaps();
    }

    void Application::SetLightingShader(const std::string& stem) {
        renderer->SetLightingShader(stem);
        static bool warned = false;
        if (stem.empty() || warned ||
            (renderer->GetFeatureCaps() & RenderEngine::CapDeferred))
            return;
        SLEAK_WARN("SetLightingShader: {} has no deferred pass, '{}' is unused",
                   renderer->GetTypeStr(), stem);
        warned = true;
    }

    const std::string& Application::GetLightingShader() const {
        return renderer->GetLightingShader();
    }
}
