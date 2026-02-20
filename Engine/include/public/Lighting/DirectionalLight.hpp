#ifndef _DIRECTIONAL_LIGHT_HPP_
#define _DIRECTIONAL_LIGHT_HPP_

#include <Lighting/Light.hpp>

namespace Sleak {

    class ENGINE_API DirectionalLight : public Light {
    public:
        explicit DirectionalLight(
            const std::string& name = "DirectionalLight");
        ~DirectionalLight() override = default;

        RenderEngine::LightGPUEntry BuildGPUData() const override;

        void SetDirection(const Math::Vector3D& dir) {
            m_direction = dir;
            m_direction.Normalize();
        }
        Math::Vector3D GetDirection() const { return m_direction; }

        // Shadow frustum configuration
        void SetShadowFrustumSize(float size) { m_shadowFrustumSize = size; }
        float GetShadowFrustumSize() const { return m_shadowFrustumSize; }

        void SetShadowDistance(float dist) { m_shadowDistance = dist; }
        float GetShadowDistance() const { return m_shadowDistance; }

        void SetShadowNearPlane(float near) { m_shadowNear = near; }
        float GetShadowNearPlane() const { return m_shadowNear; }

        void SetShadowFarPlane(float far) { m_shadowFar = far; }
        float GetShadowFarPlane() const { return m_shadowFar; }

    private:
        Math::Vector3D m_direction{0.0f, -1.0f, 0.0f};
        float m_shadowFrustumSize = 15.0f;
        float m_shadowDistance = 25.0f;
        float m_shadowNear = 0.1f;
        float m_shadowFar = 60.0f;
    };

}  // namespace Sleak

#endif  // _DIRECTIONAL_LIGHT_HPP_
