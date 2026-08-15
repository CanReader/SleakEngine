#ifndef _DIRECTIONAL_LIGHT_HPP_
#define _DIRECTIONAL_LIGHT_HPP_

#include <Lighting/Light.hpp>

namespace Sleak {

    /// Parallel-ray light with a directional shadow frustum; typically the sun/moon.
    ///
    /// A directional light has a direction but no position: every ray is
    /// parallel, so it lights the whole scene evenly. It derives from
    /// Light, which derives from GameObject, so you add it to a scene with
    /// SceneBase::AddObject() and the scene registers it with the
    /// LightManager for you.
    ///
    /// A three-light setup covers most scenes: a bright warm key that casts
    /// shadows, a dim cool fill from the opposite side that does not, and a
    /// rim light from behind to separate silhouettes from the sky. Only the
    /// key light needs SetCastShadows(true).
    ///
    /// Shadow quality lives or dies on the frustum size. It is the world
    /// extent the shadow map covers, so shrinking it to fit the area the
    /// player can actually see multiplies your effective texel density.
    /// The default covers a very large area, which is right for open
    /// terrain and far too coarse for a single figure on a small floor.
    ///
    /// @code{.cpp}
    /// auto* key = new Sleak::DirectionalLight("KeyLight");
    /// key->SetDirection(Sleak::Math::Vector3D(-0.55f, -0.80f, -0.25f));
    /// key->SetColor(1.0f, 0.93f, 0.80f);
    /// key->SetIntensity(3.0f);
    /// key->SetCastShadows(true);
    /// key->SetShadowFrustumSize(28.0f);   // tight coverage, crisp shadows
    /// key->SetShadowDistance(80.0f);
    /// key->SetShadowNearPlane(0.1f);
    /// key->SetShadowFarPlane(200.0f);
    /// key->SetShadowBias(0.0003f);
    /// key->SetShadowNormalBias(0.012f);
    /// AddObject(key);
    ///
    /// auto* fill = new Sleak::DirectionalLight("FillLight");
    /// fill->SetDirection(Sleak::Math::Vector3D(0.70f, -0.35f, 0.25f));
    /// fill->SetColor(0.72f, 0.82f, 1.00f);
    /// fill->SetIntensity(0.9f);
    /// fill->SetCastShadows(false);
    /// AddObject(fill);
    /// @endcode
    ///
    /// @see Light, LightManager, PointLight, SpotLight, AreaLight
    /// @ingroup lighting
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

        void SetShadowNearPlane(float nearPlane) { m_shadowNear = nearPlane; }
        float GetShadowNearPlane() const { return m_shadowNear; }

        void SetShadowFarPlane(float farPlane) { m_shadowFar = farPlane; }
        float GetShadowFarPlane() const { return m_shadowFar; }

    private:
        Math::Vector3D m_direction{0.0f, -1.0f, 0.0f};
        float m_shadowFrustumSize = 160.0f;
        float m_shadowDistance = 160.0f;
        float m_shadowNear = 0.1f;
        float m_shadowFar = 500.0f;
    };

}  // namespace Sleak

#endif  // _DIRECTIONAL_LIGHT_HPP_
