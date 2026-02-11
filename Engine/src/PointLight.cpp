#include <Lighting/PointLight.hpp>
#include <Graphics/ConstantBuffer.hpp>

namespace Sleak {

PointLight::PointLight(const std::string& name) : Light(name) {}

RenderEngine::LightGPUEntry PointLight::BuildGPUData() const {
    RenderEngine::LightGPUEntry entry{};

    entry.PositionX = m_position.GetX();
    entry.PositionY = m_position.GetY();
    entry.PositionZ = m_position.GetZ();
    entry.Type = 1;  // Point

    entry.DirectionX = 0.0f;
    entry.DirectionY = 0.0f;
    entry.DirectionZ = 0.0f;
    entry.Intensity = m_intensity;

    entry.ColorR = m_color.GetX();
    entry.ColorG = m_color.GetY();
    entry.ColorB = m_color.GetZ();
    entry.Range = m_range;

    entry.SpotInnerCos = 0.0f;
    entry.SpotOuterCos = 0.0f;
    entry.AreaWidth = 0.0f;
    entry.AreaHeight = 0.0f;

    return entry;
}

}  // namespace Sleak
