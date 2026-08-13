#ifndef _AREA_LIGHT_HPP_
#define _AREA_LIGHT_HPP_

#include <Lighting/Light.hpp>

namespace Sleak {

    enum class AreaLightShape : uint8_t {
        Rectangle = 0,
        Disc = 1
    };

    class ENGINE_API AreaLight : public Light {
    public:
        explicit AreaLight(const std::string& name = "AreaLight");
        ~AreaLight() override = default;

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

        void SetWidth(float width) { m_width = width; }
        float GetWidth() const { return m_width; }

        void SetHeight(float height) { m_height = height; }
        float GetHeight() const { return m_height; }

        void SetShape(AreaLightShape shape) { m_shape = shape; }
        AreaLightShape GetShape() const { return m_shape; }

        void SetTwoSided(bool twoSided) { m_twoSided = twoSided; }
        bool IsTwoSided() const { return m_twoSided; }

    private:
        Math::Vector3D m_position{0.0f, 0.0f, 0.0f};
        Math::Vector3D m_direction{0.0f, -1.0f, 0.0f};
        float m_range = 10.0f;
        float m_width = 1.0f;
        float m_height = 1.0f;
        AreaLightShape m_shape = AreaLightShape::Rectangle;
        bool m_twoSided = false;
    };

}  // namespace Sleak

#endif  // _AREA_LIGHT_HPP_
