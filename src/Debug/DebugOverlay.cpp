#include <Debug/DebugOverlay.hpp>
#include <Debug/DebugLineRenderer.hpp>
#include <Graphics/Common/Renderer.hpp>
#include <GameBase.hpp>
#include <Camera/Camera.hpp>
#include <Core/SceneBase.hpp>
#include <ECS/Components/FreeLookCameraController.hpp>
#include <UI/UI.hpp>

namespace Sleak {

DebugOverlay::~DebugOverlay() {
    DebugLineRenderer::Shutdown();
    SystemMetrics::Shutdown();
}

void DebugOverlay::Initialize(RenderEngine::Renderer* renderer,
                              GameBase* game) {
    m_renderer = renderer;
    m_game = game;
    SystemMetrics::Initialize();
    DebugLineRenderer::Initialize();
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
        camera = m_game->GetActiveScene()->GetActiveCamera();
    if (!camera) return;

    UI::BeginPanel("Camera", 0, 0, m_config.PanelAlpha,
                   UI::PanelFlags_AutoResize);

    UI::Text("%s", camera->GetName().c_str());
    UI::Text("Position:  %s",
             camera->GetPosition().ToString().c_str());
    UI::Text("Direction: %s",
             camera->GetDirection().ToString().c_str());
    UI::Text("Look At:   %s",
             camera->GetLookTarget().ToString().c_str());
    UI::Text("Up:        %s",
             camera->GetUp().ToString().c_str());

    float fov = camera->GetFieldOfView();
    if (UI::DragFloat("FOV", &fov, 1.0f, 30.0f, 125.0f))
        camera->SetFieldOfView(fov);

    const char* projStr =
        (camera->GetProjectionType() == ProjectionType::Perspective)
            ? "Perspective"
            : "Orthographic";
    UI::Text("Projection: %s", projStr);
    UI::Text("Near: %.2f  Far: %.2f", camera->GetNearPlane(),
             camera->GetFarPlane());

    UI::BeginGroup();
    if (UI::Button("To Origin"))
        camera->SetPosition({0, 0, -5});
    UI::SameLine();
    if (UI::Button("Look Forward"))
        camera->SetDirection({0, 0, 1});
    UI::EndGroup();

    UI::Separator();

    // FreeLookCameraController section
    auto* controller =
        camera->GetComponent<FreeLookCameraController>();
    if (controller) {
        UI::BeginChild("Camera Controller");

        bool enabled = controller->IsEnabled();
        if (enabled)
            UI::TextColored(0, 1, 0, 1, "Camera control Enabled");
        else
            UI::TextColored(1, 0, 0, 1, "Camera control Disabled");

        float yaw = controller->GetYaw();
        if (UI::DragFloat(
                "Yaw", &yaw, 1.0f,
                controller->GetYawRange().GetX(),
                controller->GetYawRange().GetY()))
            controller->SetYaw(yaw);

        float pitch = controller->GetPitch();
        if (UI::DragFloat(
                "Pitch", &pitch, 1.0f,
                controller->GetPitchRange().GetX(),
                controller->GetPitchRange().GetY()))
            controller->SetPitch(pitch);

        float roll = controller->GetRoll();
        if (UI::DragFloat(
                "Roll", &roll, 1.0f,
                controller->GetRollRange().GetX(),
                controller->GetRollRange().GetY()))
            controller->SetRoll(roll);

        UI::Text("Speed: %.2f / %.2f", controller->GetSpeed(),
                 controller->GetMaxSpeed());
        UI::Text("Acceleration: %.2f",
                 controller->GetAcceleration());
        UI::Text("Velocity: %s",
                 controller->GetVelocity().ToString().c_str());
        UI::Text("Input: %s",
                 controller->GetTranslationInput()
                     .ToString()
                     .c_str());

        UI::EndChild();
    }

    UI::EndPanel();
}

void DebugOverlay::RenderPerformancePanel() {
    float viewportWidth = UI::GetViewportWidth();

    UI::BeginPanel("Performance", viewportWidth - 240, 0,
                   m_config.PanelAlpha, UI::PanelFlags_AutoResize);

    // Color-coded renderer type
    switch (m_renderer->GetType()) {
        case RenderEngine::RendererType::DirectX12:
            UI::TextColored(0.0f, 0.5f, 1.0f, 1.0f, "DirectX 12");
            break;
        case RenderEngine::RendererType::DirectX11:
            UI::TextColored(0.2f, 0.6f, 0.8f, 1.0f, "DirectX 11");
            break;
        case RenderEngine::RendererType::Vulkan:
            UI::TextColored(0.8f, 0.2f, 0.0f, 1.0f, "Vulkan");
            break;
        case RenderEngine::RendererType::OpenGL:
            UI::TextColored(0.0f, 0.8f, 0.2f, 1.0f, "OpenGL");
            break;
        default:
            UI::TextColored(0.5f, 0.5f, 0.5f, 1.0f, "Unknown");
            break;
    }

    UI::Separator();
    UI::Text("FPS: %d", m_renderer->GetFrameRate());
    UI::Text("Frame Time: %.2f ms", m_renderer->GetFrameTime());

    UI::Separator();
    UI::Text("Vertices:  %d", m_renderer->GetVertices());
    UI::Text("Triangles: %d", m_renderer->GetTriangles());

    UI::Separator();
    UI::Text("CPU: %.1f%%", m_cachedMetrics.CpuUsagePercent);
    UI::Text("RAM: %.1f MB", m_cachedMetrics.RamUsageMB);

    if (m_cachedMetrics.GpuUsagePercent > 0.0f)
        UI::Text("GPU: %.1f%%", m_cachedMetrics.GpuUsagePercent);
    else
        UI::TextDisabled("GPU: N/A");

    UI::Separator();
    if (UI::Checkbox("Show Colliders", &m_showColliders)) {
        DebugLineRenderer::SetEnabled(m_showColliders);
    }

    UI::Separator();
    UI::Text("Anti-Aliasing");
    {
        const char* labels[] = {"Off", "2x", "4x", "8x"};
        int values[] = {1, 2, 4, 8};
        int count = 1;
        uint32_t maxMSAA = m_renderer->GetMaxMSAASampleCount();
        for (int i = 1; i < 4; i++)
            if (static_cast<uint32_t>(values[i]) <= maxMSAA) count = i + 1;
        int current = 0;
        for (int i = 0; i < count; i++)
            if (static_cast<uint32_t>(values[i]) == m_renderer->GetMSAASampleCount()) current = i;
        if (UI::Combo("MSAA", &current, labels, count))
            m_renderer->SetMSAASampleCount(values[current]);
    }

    UI::EndPanel();
}

}  // namespace Sleak
