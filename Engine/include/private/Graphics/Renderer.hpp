#pragma once


#include "RenderContext.hpp"
#include <Core/OSDef.hpp>

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
        return DrawnVertices;
    }

    inline int GetTriangles() const {
        return DrawnTriangles;
    }

    inline bool GetIsPerformanceCounter() {
        return bEnabledPerformanceCounter;
    }

    inline void SetPerformanceCounter(bool value) {
        bEnabledPerformanceCounter = value;
    }

    inline void SetImGUI(bool bEnable) {bImInitialized = bEnable;}
    inline bool GetImGUIEnabled() { return bImInitialized; }

    protected:
    virtual void ConfigureRenderMode() = 0;
    virtual void ConfigureRenderFace() = 0;
    
    // Performance counter
    bool bEnabledPerformanceCounter = false;
    int frameRate = 0;
    float frameTime = 0;
    float UsedRam = 0;
    float UsedCPU = 0;
    int DrawnVertices = 0;
    int DrawnTriangles = 0;

    // ImGUI
    bool bImInitialized = false;

    // Renderer
    RendererType Type;
    RenderMode Mode;
    RenderFace Face;

    static constexpr int FrameTimeHistory = 100;
};

inline Renderer::~Renderer() {} // Provide definition for pure virtual destructor

}
}

