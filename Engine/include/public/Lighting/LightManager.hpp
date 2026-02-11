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

    private:
        List<Light*> m_lights;

        RefPtr<RenderEngine::BufferBase> m_lightBuffer;

        float m_ambientR = 0.1f;
        float m_ambientG = 0.1f;
        float m_ambientB = 0.1f;
        float m_ambientIntensity = 1.0f;
    };

}  // namespace Sleak

#endif  // _LIGHT_MANAGER_HPP_
