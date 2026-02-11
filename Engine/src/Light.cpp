#include <Lighting/Light.hpp>

namespace Sleak {

Light::Light(const std::string& name) : GameObject(name) {}

void Light::SetColor(float r, float g, float b) {
    m_color = Math::Vector3D(r, g, b);
}

void Light::SetColor(const Math::Color& color) {
    Math::Vector4D norm = color.normalize();
    m_color = Math::Vector3D(norm.GetX(), norm.GetY(), norm.GetZ());
}

}  // namespace Sleak
