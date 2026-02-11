#include <Debug/DebugOverlay.hpp>
#include <Graphics/Renderer.hpp>
#include <GameBase.hpp>
#include <Camera/Camera.hpp>
#include <Core/SceneBase.hpp>
#include <ECS/Components/FreeLookCameraController.hpp>
#include <imgui.h>

namespace Sleak {

DebugOverlay::~DebugOverlay() {
    SystemMetrics::Shutdown();
}

void DebugOverlay::Initialize(RenderEngine::Renderer* renderer,
                              GameBase* game) {
    m_renderer = renderer;
    m_game = game;
    SystemMetrics::Initialize();
}

void DebugOverlay::Render(float deltaTime) {
    if (!m_renderer || !m_renderer->GetImGUIEnabled() || !m_visible)
        return;

    // Refresh cached metrics on interval
    m_metricTimer += deltaTime;
    if (m_metricTimer >= m_config.MetricRefreshInterval) {
        m_cachedMetrics = SystemMetrics::Query();
        m_metricTimer = 0.0f;
    }

    if (m_config.ShowCameraPanel) RenderCameraPanel();
    if (m_config.ShowPerformancePanel) RenderPerformancePanel();
}

void DebugOverlay::RenderCameraPanel() {
    Camera* camera = nullptr;
    if (m_game && m_game->GetActiveScene())
        camera = m_game->GetActiveScene()->GetDebugCamera();
    if (!camera) return;

    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(m_config.PanelAlpha);
    ImGui::Begin("Camera", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("%s", camera->GetName().c_str());
    ImGui::Text("Position:  %s",
                camera->GetPosition().ToString().c_str());
    ImGui::Text("Direction: %s",
                camera->GetDirection().ToString().c_str());
    ImGui::Text("Look At:   %s",
                camera->GetLookTarget().ToString().c_str());
    ImGui::Text("Up:        %s",
                camera->GetUp().ToString().c_str());

    float fov = camera->GetFieldOfView();
    if (ImGui::DragFloat("FOV", &fov, 1.0f, 30.0f, 125.0f))
        camera->SetFieldOfView(fov);

    const char* projStr =
        (camera->GetProjectionType() == ProjectionType::Perspective)
            ? "Perspective"
            : "Orthographic";
    ImGui::Text("Projection: %s", projStr);
    ImGui::Text("Near: %.2f  Far: %.2f", camera->GetNearPlane(),
                camera->GetFarPlane());

    ImGui::BeginGroup();
    if (ImGui::Button("To Origin"))
        camera->SetPosition({0, 0, -5});
    ImGui::SameLine();
    if (ImGui::Button("Look Forward"))
        camera->SetDirection({0, 0, 1});
    ImGui::EndGroup();

    ImGui::Separator();

    // FreeLookCameraController section
    auto* controller =
        camera->GetComponent<FreeLookCameraController>();
    if (controller) {
        ImGui::BeginChild("Camera Controller", {0, 0},
                          ImGuiChildFlags_AutoResizeY);

        bool enabled = controller->IsEnabled();
        ImVec4 color = enabled ? ImVec4(0, 1, 0, 1)
                               : ImVec4(1, 0, 0, 1);
        ImGui::TextColored(color, "Camera control %s",
                           enabled ? "Enabled" : "Disabled");

        float yaw = controller->GetYaw();
        if (ImGui::DragFloat(
                "Yaw", &yaw, 1.0f,
                controller->GetYawRange().GetX(),
                controller->GetYawRange().GetY()))
            controller->SetYaw(yaw);

        float pitch = controller->GetPitch();
        if (ImGui::DragFloat(
                "Pitch", &pitch, 1.0f,
                controller->GetPitchRange().GetX(),
                controller->GetPitchRange().GetY()))
            controller->SetPitch(pitch);

        float roll = controller->GetRoll();
        if (ImGui::DragFloat(
                "Roll", &roll, 1.0f,
                controller->GetRollRange().GetX(),
                controller->GetRollRange().GetY()))
            controller->SetRoll(roll);

        ImGui::Text("Speed: %.2f / %.2f", controller->GetSpeed(),
                    controller->GetMaxSpeed());
        ImGui::Text("Acceleration: %.2f",
                    controller->GetAcceleration());
        ImGui::Text("Velocity: %s",
                    controller->GetVelocity().ToString().c_str());
        ImGui::Text("Input: %s",
                    controller->GetTranslationInput()
                        .ToString()
                        .c_str());

        ImGui::EndChild();
    }

    ImGui::End();
}

void DebugOverlay::RenderPerformancePanel() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 viewportSize = ImGui::GetMainViewport()->Size;

    ImGui::SetNextWindowPos({viewportSize.x - 240, 0},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(m_config.PanelAlpha);
    ImGui::Begin("Performance", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize);

    // Color-coded renderer type
    switch (m_renderer->GetType()) {
        case RenderEngine::RendererType::DirectX12:
            ImGui::TextColored({0.0f, 0.5f, 1.0f, 1.0f},
                               "DirectX 12");
            break;
        case RenderEngine::RendererType::DirectX11:
            ImGui::TextColored({0.2f, 0.6f, 0.8f, 1.0f},
                               "DirectX 11");
            break;
        case RenderEngine::RendererType::Vulkan:
            ImGui::TextColored({0.8f, 0.2f, 0.0f, 1.0f},
                               "Vulkan");
            break;
        case RenderEngine::RendererType::OpenGL:
            ImGui::TextColored({0.0f, 0.8f, 0.2f, 1.0f},
                               "OpenGL");
            break;
        default:
            ImGui::TextColored({0.5f, 0.5f, 0.5f, 1.0f},
                               "Unknown");
            break;
    }

    ImGui::Separator();
    ImGui::Text("FPS: %d", m_renderer->GetFrameRate());
    ImGui::Text("Frame Time: %.2f ms",
                m_renderer->GetFrameTime());

    ImGui::Separator();
    ImGui::Text("Vertices:  %d", m_renderer->GetVertices());
    ImGui::Text("Triangles: %d", m_renderer->GetTriangles());

    ImGui::Separator();
    ImGui::Text("CPU: %.1f%%", m_cachedMetrics.CpuUsagePercent);
    ImGui::Text("RAM: %.1f MB", m_cachedMetrics.RamUsageMB);

    if (m_cachedMetrics.GpuUsagePercent > 0.0f)
        ImGui::Text("GPU: %.1f%%",
                     m_cachedMetrics.GpuUsagePercent);
    else
        ImGui::TextDisabled("GPU: N/A");

    ImGui::End();
}

}  // namespace Sleak
