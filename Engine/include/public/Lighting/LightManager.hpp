#ifndef _LIGHT_MANAGER_HPP_
#define _LIGHT_MANAGER_HPP_

#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>
#include <Math/Vector.hpp>
#include <Memory/RefPtr.h>

namespace Sleak {

    class Light;

    namespace RenderEngine {
        class BufferBase;
        struct LightCBData;
    }

    class ENGINE_API LightManager {
    public:
        LightManager();
        ~LightManager();

        void Initialize();

        void RegisterLight(Light* light);
        void UnregisterLight(Light* light);

        void UpdateAndBind();
        void UpdateShadowData();
        void UpdateDeferredCB();

        void SetAmbientColor(float r, float g, float b);
        void SetAmbientIntensity(float intensity) {
            m_ambientIntensity = intensity;
        }

        float GetAmbientIntensity() const {
            return m_ambientIntensity;
        }

        size_t GetLightCount() const {
            return m_lights.GetSize();
        }

        void SetFogColor(float r, float g, float b) {
            m_fogR = r; m_fogG = g; m_fogB = b;
        }
        void SetFogDistances(float start, float end) {
            m_fogStart = start; m_fogEnd = end;
        }
        void SetFogEnabled(bool enabled) { m_fogEnabled = enabled; }
        bool IsFogEnabled() const { return m_fogEnabled; }
        float GetFogStart() const { return m_fogStart; }
        float GetFogEnd() const { return m_fogEnd; }

    private:
        List<Light*> m_lights;

        RefPtr<RenderEngine::BufferBase> m_lightBuffer;

        float m_ambientR = 0.1f;
        float m_ambientG = 0.1f;
        float m_ambientB = 0.1f;
        float m_ambientIntensity = 1.0f;

        // Fog
        bool  m_fogEnabled = true;
        float m_fogR = 0.3f, m_fogG = 0.4f, m_fogB = 1.0f;
        float m_fogStart = 80.0f;
        float m_fogEnd = 128.0f;
    };

}  // namespace Sleak

#endif  // _LIGHT_MANAGER_HPP_
