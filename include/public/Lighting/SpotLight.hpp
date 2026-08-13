#ifndef _SPOT_LIGHT_HPP_
#define _SPOT_LIGHT_HPP_

#include <Lighting/Light.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Sleak {

    class ENGINE_API SpotLight : public Light {
    public:
        explicit SpotLight(const std::string& name = "SpotLight");
        ~SpotLight() override = default;

        RenderEngine::LightGPUEntry BuildGPUData() const override;

        void SetPosition(const Math::Vector3D& pos) {
            m_position = pos;
        }
        Math::Vector3D GetPosition() const { return m_position; }

        void SetDirection(const Math::Vector3D& dir) {
            m_direction = dir;
            m_direction.Normalize();
        }
        Math::Vector3D GetDirection() const { return m_direction; }

        void SetRange(float range) { m_range = range; }
        float GetRange() const { return m_range; }

        // Angles in degrees
        void SetInnerConeAngle(float degrees) {
            m_innerAngleDeg = degrees;
        }
        float GetInnerConeAngle() const { return m_innerAngleDeg; }

        void SetOuterConeAngle(float degrees) {
            m_outerAngleDeg = degrees;
        }
        float GetOuterConeAngle() const { return m_outerAngleDeg; }

    private:
        Math::Vector3D m_position{0.0f, 0.0f, 0.0f};
        Math::Vector3D m_direction{0.0f, -1.0f, 0.0f};
        float m_range = 10.0f;
        float m_innerAngleDeg = 20.0f;
        float m_outerAngleDeg = 30.0f;
    };

}  // namespace Sleak

#endif  // _SPOT_LIGHT_HPP_
