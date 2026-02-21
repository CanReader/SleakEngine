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

    // Renderer
    RendererType Type;
    RenderMode Mode;
    RenderFace Face;

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

