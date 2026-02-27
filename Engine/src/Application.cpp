#include "../../include/public/Core/Application.hpp"
#include "../../include/private/Graphics/RendererFactory.hpp" 
#include "../../include/private/Graphics/RenderCommandQueue.hpp" 
#include <WindowHelper.hpp>
#include <Graphics/Renderer.hpp>
#include <Window.hpp>
#include <cstring>
#include <exception>
#include <stdexcept>
#include "Graphics/Vulkan/VulkanRenderer.hpp"
#include "Logger.hpp"
#include <Memory/ObjectPtr.h>

#include <Runtime/InternalGeometry.hpp>
#include <Camera/Camera.hpp>
#include <Core/GameObject.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Random.hpp>
#include <Utility/Container/List.hpp>
#include <Core/ScopedTimer.h>
#include <Math/Matrix.hpp>
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

        int width = 1200, height = 800;
        if (Specification.CommandLineArgs.Size > 1) {
            if (!Specification.CommandLineArgs["-w"].empty())
                width = std::stoi(Specification.CommandLineArgs["-w"]);
            if (!Specification.CommandLineArgs["-h"].empty())
                height = std::stoi(Specification.CommandLineArgs["-h"]);
            if (!Specification.CommandLineArgs["-t"].empty())
                Specification.Name = Specification.CommandLineArgs["-t"];
        }

        CoreWindow = new Window(width,height,Specification.Name);
        
        try {
            if (!Specification.CommandLineArgs["-r"].empty())
            {
                renderer = RenderEngine::
                           RendererFactory::
                           ParseArg(Specification.CommandLineArgs["-r"], CoreWindow);
            }
            else {
                #ifdef PLATFORM_WIN
                renderer = RenderEngine::RendererFactory::CreateRenderer(
                    RenderEngine::RendererType::DirectX11, CoreWindow);
                #else
                    renderer = RenderEngine::RendererFactory::CreateRenderer(RenderEngine::RendererType::Vulkan, CoreWindow);
                #endif
                }
        } 
        catch(std::exception& e) {
            SLEAK_ERROR(std::string(e.what()));
            renderer = RenderEngine::RendererFactory::CreateRenderer(RenderEngine::RendererType::Vulkan, CoreWindow);
        }

        EventDispatcher::RegisterEventHandler(this,&Application::OnKeyPressed);
        EventDispatcher::RegisterEventHandler(this, &Application::OnWindowResize);
        EventDispatcher::RegisterEventHandler(this, &Application::OnWindowFullScreen);
        
    }

    Application::~Application() {
        // Wait for GPU to finish before destroying any resources
        if (renderer)
            renderer->Cleanup();

        // Game must be deleted first — scene cleanup destroys objects
        // whose components hold render resources (buffers, etc.)
        delete Game;
        delete m_DebugOverlay;
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
            }

            while(!CoreWindow->ShouldClose()) {
                
                float currentTime = FrameTimer.Elapsed();
                DeltaTime = currentTime - lastTime;
                lastTime = currentTime;
                
                #if defined(_DEBUG) && defined(COUNT_FRAME)
                    ScopedTimer("Frame Timer");
                #endif

                CoreWindow->Update();

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
        renderer->Resize(e.GetWidth(), e.GetHeight());

        width = e.GetWidth();
        height = e.GetHeight();
    }

    void Application::OnWindowFullScreen(const Sleak::Events::WindowFullScreen& e) {
        int w = Window::GetWidth();
        int h = Window::GetHeight();
        if (w > 0 && h > 0)
            renderer->Resize(w, h);
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
                    Camera* cam = Game->GetActiveScene()->GetDebugCamera();
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

        }

    }

    Window& Application::GetWindow() {
        return *CoreWindow;
    }

    int Application::GetFPS() const { return renderer->GetFrameRate(); }
    float Application::GetFrameTime() const { return renderer->GetFrameTime(); }
    int Application::GetVertices() const { return renderer->GetVertices(); }
    int Application::GetTriangles() const { return renderer->GetTriangles(); }
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

}
