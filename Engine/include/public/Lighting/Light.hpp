#ifndef _LIGHT_HPP_
#define _LIGHT_HPP_

#include <Core/GameObject.hpp>
#include <Math/Vector.hpp>
#include <Math/Color.hpp>
#include <cmath>

namespace Sleak {

    namespace RenderEngine {
        struct LightGPUEntry;
    }

    class ENGINE_API Light : public GameObject {
    public:
        explicit Light(const std::string& name = "Light");
        ~Light() override = default;

        bool IsLight() const override { return true; }

        // Build GPU data for the lighting constant buffer
        virtual RenderEngine::LightGPUEntry BuildGPUData() const = 0;

        // --- Properties ---

        void SetColor(float r, float g, float b);
        void SetColor(const Math::Color& color);
        Math::Vector3D GetColor() const { return m_color; }

        void SetIntensity(float intensity) {
            m_intensity = intensity;
        }
        float GetIntensity() const { return m_intensity; }

        void SetEnabled(bool enabled) { m_lightEnabled = enabled; }
        bool IsEnabled() const { return m_lightEnabled; }

        void SetCastShadows(bool cast) { m_castShadows = cast; }
        bool GetCastShadows() const { return m_castShadows; }

        void SetShadowBias(float bias) { m_shadowBias = bias; }
        float GetShadowBias() const { return m_shadowBias; }

        void SetShadowStrength(float strength) {
            m_shadowStrength = strength;
        }
        float GetShadowStrength() const { return m_shadowStrength; }

    protected:
        Math::Vector3D m_color{1.0f, 1.0f, 1.0f};
        float m_intensity = 1.0f;
        bool m_lightEnabled = true;
        bool m_castShadows = false;
        float m_shadowBias = 0.005f;
        float m_shadowStrength = 1.0f;
    };

}  // namespace Sleak

#endif  // _LIGHT_HPP_
