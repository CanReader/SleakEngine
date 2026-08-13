#ifndef _POINT_LIGHT_HPP_
#define _POINT_LIGHT_HPP_

#include <Lighting/Light.hpp>

namespace Sleak {

    class ENGINE_API PointLight : public Light {
    public:
        explicit PointLight(const std::string& name = "PointLight");
        ~PointLight() override = default;

        RenderEngine::LightGPUEntry BuildGPUData() const override;

        void SetRange(float range) { m_range = range; }
        float GetRange() const { return m_range; }

        void SetPosition(const Math::Vector3D& pos) {
            m_position = pos;
        }
        Math::Vector3D GetPosition() const { return m_position; }

    private:
        Math::Vector3D m_position{0.0f, 0.0f, 0.0f};
        float m_range = 10.0f;
    };

}  // namespace Sleak

#endif  // _POINT_LIGHT_HPP_
