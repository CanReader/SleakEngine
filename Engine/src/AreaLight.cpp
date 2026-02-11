#include <Lighting/AreaLight.hpp>
#include <Graphics/ConstantBuffer.hpp>

namespace Sleak {

AreaLight::AreaLight(const std::string& name) : Light(name) {}

RenderEngine::LightGPUEntry AreaLight::BuildGPUData() const {
    RenderEngine::LightGPUEntry entry{};

    entry.PositionX = m_position.GetX();
    entry.PositionY = m_position.GetY();
    entry.PositionZ = m_position.GetZ();
    entry.Type = 3;  // Area

    entry.DirectionX = m_direction.GetX();
    entry.DirectionY = m_direction.GetY();
    entry.DirectionZ = m_direction.GetZ();
    entry.Intensity = m_intensity;

    entry.ColorR = m_color.GetX();
    entry.ColorG = m_color.GetY();
    entry.ColorB = m_color.GetZ();
    entry.Range = m_range;

    entry.SpotInnerCos = 0.0f;
    entry.SpotOuterCos = 0.0f;
    entry.AreaWidth = m_width;
    entry.AreaHeight = m_height;

    return entry;
}

}  // namespace Sleak
