#pragma once

#include <Core/OSDef.hpp>
#include <Debug/SystemMetrics.hpp>

namespace Sleak {

class GameBase;
namespace RenderEngine { class Renderer; }

struct DebugOverlayConfig {
    bool ShowCameraPanel = true;
    bool ShowPerformancePanel = true;
    float PanelAlpha = 0.85f;
    float MetricRefreshInterval = 0.5f;
};

class ENGINE_API DebugOverlay {
public:
    DebugOverlay() = default;
    ~DebugOverlay();

    void Initialize(RenderEngine::Renderer* renderer,
                    GameBase* game);
    void Render(float deltaTime);

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }
    void ToggleVisible() { m_visible = !m_visible; }

    DebugOverlayConfig& GetConfig() { return m_config; }

private:
    void RenderCameraPanel();
    void RenderPerformancePanel();

    RenderEngine::Renderer* m_renderer = nullptr;
    GameBase* m_game = nullptr;

    bool m_visible = true;
    bool m_showColliders = false;
    DebugOverlayConfig m_config;

    // Cached metrics
    SystemMetricsData m_cachedMetrics;
    float m_metricTimer = 0.0f;
};

}  // namespace Sleak
