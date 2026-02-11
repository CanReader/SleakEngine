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

    private:
        Math::Vector3D m_direction{0.0f, -1.0f, 0.0f};
    };

}  // namespace Sleak

#endif  // _DIRECTIONAL_LIGHT_HPP_
