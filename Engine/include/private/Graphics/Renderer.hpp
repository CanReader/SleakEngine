#pragma once


#include "RenderContext.hpp"
#include <Core/OSDef.hpp>
#include <Core/Timer.hpp>

namespace Sleak {

namespace RenderEngine {
    
enum class RendererType {
    Vulkan,
    OpenGL,
    DirectX11,
    DirectX12
};

class ENGINE_API Renderer {
public:
    virtual ~Renderer() = 0; // Pure virtual destructor

    virtual bool Initialize() = 0;
    virtual void BeginRender() = 0;
    virtual void EndRender() = 0;
    virtual void Cleanup() = 0;
    virtual void WaitIdle() {}
    virtual void FlushPendingTransfers() {}

    virtual void Resize(uint32_t width, uint32_t height) = 0;

    virtual bool CreateImGUI() = 0;

    virtual RenderContext* GetContext() = 0;

    inline RendererType GetType() const
    {
        return Type;
    }

    inline const char* GetTypeStr() const  {
        switch(GetType())
        {
            case RendererType::DirectX12: return "DirectX 12";
            case RendererType::DirectX11: return "DirectX 11";
            case RendererType::Vulkan:    return "Vulkan";
            case RendererType::OpenGL:    return "OpenGL";
            default: return "Unknown!";
        }
    }

    inline RenderMode GetRenderMode() const {
        return Mode;
    }

    inline void SetRenderDrawMode(RenderMode mode) {
        Mode = mode;
        ConfigureRenderMode();
    }

    inline void SetRenderCullFace(RenderFace face) {
        Face = face;
        ConfigureRenderFace();
    }

    inline int GetFrameRate() const {
        return frameRate;
    }

    inline float GetFrameTime() const {
        return frameTime;
    }

    inline float GetRamUsage() const {
        return UsedRam;
    }

    inline float GetCPUUsage() const {
        return UsedCPU;
    }

    inline int GetVertices() const {
        return DisplayVertices;
    }

    inline int GetTriangles() const {
        return DisplayTriangles;
    }

    inline bool GetIsPerformanceCounter() {
        return bEnabledPerformanceCounter;
    }

    inline void SetPerformanceCounter(bool value) {
        bEnabledPerformanceCounter = value;
    }

    inline void SetImGUI(bool bEnable) {bImInitialized = bEnable;}
    inline bool GetImGUIEnabled() { return bImInitialized; }

    // Shadow mapping support (overridden by VulkanRenderer)
    virtual void UpdateShadowLightUBO(const void* data, uint32_t size) { (void)data; (void)size; }
    virtual void SetLightVP(const float* lightVP) { (void)lightVP; }
    void SetShadowPassEnabled(bool enabled) { m_shadowPassEnabled = enabled; }
    bool IsShadowPassEnabled() const { return m_shadowPassEnabled; }

    // VSync
    virtual void SetVSync(bool enabled) {
        if (enabled == m_vsync) return;
        m_vsync = enabled;
        m_vsyncChangeRequested = true;
    }
    bool GetVSync() const { return m_vsync; }
    virtual void ApplyVSyncChange() {}

    // Post-process: ACES Tonemapping
    void SetTonemappingEnabled(bool enabled) { m_tonemapEnabled = enabled; }
    bool IsTonemappingEnabled() const { return m_tonemapEnabled; }
    void SetExposure(float exposure) { m_exposure = exposure; }
    float GetExposure() const { return m_exposure; }
    void SetGamma(float gamma) { m_gamma = gamma; }
    float GetGamma() const { return m_gamma; }

    // Post-process: SSAO
    void SetSSAOEnabled(bool enabled) { m_ssaoEnabled = enabled; }
    bool IsSSAOEnabled() const { return m_ssaoEnabled; }
    void SetSSAORadius(float radius) { m_ssaoRadius = radius; }
    float GetSSAORadius() const { return m_ssaoRadius; }
    void SetSSAOBias(float bias) { m_ssaoBias = bias; }
    float GetSSAOBias() const { return m_ssaoBias; }
    void SetSSAOPower(float power) { m_ssaoPower = power; }
    float GetSSAOPower() const { return m_ssaoPower; }

    // IBL (Image Based Lighting)
    void SetIBLEnabled(bool enabled) { m_iblEnabled = enabled; }
    bool IsIBLEnabled() const { return m_iblEnabled; }
    void SetIBLIntensity(float intensity) { m_iblIntensity = intensity; }
    float GetIBLIntensity() const { return m_iblIntensity; }

    // PCSS Soft Shadows
    void SetPCSSEnabled(bool enabled) { m_pcssEnabled = enabled; }
    bool IsPCSSEnabled() const { return m_pcssEnabled; }

    // Anti-aliasing (MSAA)
    virtual void SetMSAASampleCount(uint32_t samples) {
        // Validate: must be 1, 2, 4, or 8
        if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
            return;
        if (samples > m_maxMsaaSampleCount)
            samples = m_maxMsaaSampleCount;
        if (samples == m_msaaSampleCount)
            return;
        m_pendingMsaaSampleCount = samples;
        m_msaaChangeRequested = true;
    }
    uint32_t GetMSAASampleCount() const { return m_msaaSampleCount; }
    uint32_t GetMaxMSAASampleCount() const { return m_maxMsaaSampleCount; }
    virtual void ApplyMSAAChange() {}

    protected:
    virtual void ConfigureRenderMode() = 0;
    virtual void ConfigureRenderFace() = 0;

    void UpdateFrameMetrics() {
        if (!bEnabledPerformanceCounter) return;
        m_frameCount++;
        float elapsed = m_frameTimer.Elapsed(); // seconds since last reset
        if (elapsed >= MetricUpdateInterval) {
            frameRate = static_cast<int>(m_frameCount / elapsed);
            frameTime = (elapsed / m_frameCount) * 1000.0f; // ms per frame
            DisplayVertices = DrawnVertices;
            DisplayTriangles = DrawnTriangles;
            m_frameCount = 0;
            m_frameTimer.Reset();
            DrawnVertices = 0;
            DrawnTriangles = 0;
        }
    }

    // Performance counter
    bool bEnabledPerformanceCounter = false;
    int frameRate = 0;
    float frameTime = 0;
    float UsedRam = 0;
    float UsedCPU = 0;
    int DrawnVertices = 0;
    int DrawnTriangles = 0;
    int DisplayVertices = 0;
    int DisplayTriangles = 0;
    Timer m_frameTimer;
    uint32_t m_frameCount = 0;

    // ImGUI
    bool bImInitialized = false;
    bool bImFrameActive = false;  // true when ImGui::NewFrame was called this frame

    // Renderer
    RendererType Type;
    RenderMode Mode;
    RenderFace Face;

    // Shadow pass
    bool m_shadowPassEnabled = false;

    // Post-process: Tonemapping
    bool m_tonemapEnabled = true;
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    // Post-process: SSAO
    bool m_ssaoEnabled = false;
    float m_ssaoRadius = 0.5f;
    float m_ssaoBias = 0.025f;
    float m_ssaoPower = 2.0f;

    // IBL
    bool m_iblEnabled = false;
    float m_iblIntensity = 1.0f;

    // PCSS
    bool m_pcssEnabled = true;

    // VSync state
    bool m_vsync = false;
    bool m_vsyncChangeRequested = false;

    // MSAA state
    uint32_t m_msaaSampleCount = 1;
    uint32_t m_maxMsaaSampleCount = 1;
    bool m_msaaChangeRequested = false;
    uint32_t m_pendingMsaaSampleCount = 1;

    static constexpr float MetricUpdateInterval = 0.5f; // seconds
};

inline Renderer::~Renderer() {} // Provide definition for pure virtual destructor

}
}

