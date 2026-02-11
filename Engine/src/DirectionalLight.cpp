#include <Lighting/DirectionalLight.hpp>
#include <Graphics/ConstantBuffer.hpp>

namespace Sleak {

DirectionalLight::DirectionalLight(const std::string& name)
    : Light(name) {}

RenderEngine::LightGPUEntry DirectionalLight::BuildGPUData() const {
    RenderEngine::LightGPUEntry entry{};

    // Directional lights have no position
    entry.PositionX = 0.0f;
    entry.PositionY = 0.0f;
    entry.PositionZ = 0.0f;
    entry.Type = 0;  // Directional

    entry.DirectionX = m_direction.GetX();
    entry.DirectionY = m_direction.GetY();
    entry.DirectionZ = m_direction.GetZ();
    entry.Intensity = m_intensity;

    entry.ColorR = m_color.GetX();
    entry.ColorG = m_color.GetY();
    entry.ColorB = m_color.GetZ();
    entry.Range = 0.0f;  // Not used for directional

    entry.SpotInnerCos = 0.0f;
    entry.SpotOuterCos = 0.0f;
    entry.AreaWidth = 0.0f;
    entry.AreaHeight = 0.0f;

    return entry;
}

}  // namespace Sleak
