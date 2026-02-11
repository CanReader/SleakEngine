#include <Lighting/SpotLight.hpp>
#include <Graphics/ConstantBuffer.hpp>

namespace Sleak {

SpotLight::SpotLight(const std::string& name) : Light(name) {}

RenderEngine::LightGPUEntry SpotLight::BuildGPUData() const {
    RenderEngine::LightGPUEntry entry{};

    entry.PositionX = m_position.GetX();
    entry.PositionY = m_position.GetY();
    entry.PositionZ = m_position.GetZ();
    entry.Type = 2;  // Spot

    entry.DirectionX = m_direction.GetX();
    entry.DirectionY = m_direction.GetY();
    entry.DirectionZ = m_direction.GetZ();
    entry.Intensity = m_intensity;

    entry.ColorR = m_color.GetX();
    entry.ColorG = m_color.GetY();
    entry.ColorB = m_color.GetZ();
    entry.Range = m_range;

    // Convert degrees to cosines for GPU
    float innerRad =
        m_innerAngleDeg * static_cast<float>(M_PI) / 180.0f;
    float outerRad =
        m_outerAngleDeg * static_cast<float>(M_PI) / 180.0f;
    entry.SpotInnerCos = std::cos(innerRad);
    entry.SpotOuterCos = std::cos(outerRad);
    entry.AreaWidth = 0.0f;
    entry.AreaHeight = 0.0f;

    return entry;
}

}  // namespace Sleak
