#include <Core/Logger.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "../../include/private/Graphics/Common/CloudNoise.hpp"
#include "../../include/private/Graphics/Common/Renderer.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

// Powers of bases that reach zero must stay positive
constexpr float EXPONENT_MIN = 1e-3f;
// The glare lobe divides by its width and by one minus it
constexpr float GLARE_WIDTH_MIN = 1e-3f;
constexpr float GLARE_WIDTH_MAX = 0.999f;
// Height fog halves every 128 m above its base
constexpr float FOG_HEIGHT_FALLOFF = 1.0f / 128.0f;

/// Writes a vector and a fourth value into one std140 row.
void SetRow(float row[4], const Math::Vector3D& xyz, float w = 0.0f) {
    row[0] = xyz.GetX();
    row[1] = xyz.GetY();
    row[2] = xyz.GetZ();
    row[3] = w;
}

}  // namespace

void Renderer::BuildFrameAtmosphere(AtmosphereData& out) const {
    out = {};
    out.Header[0] = ATMOSPHERE_DATA_VERSION;
    const bool composited = DrawsAtmosphere();
    if (!composited && !DrawsForwardSky()) return;
    // The forward sky reads the rows alone; fog and the stems need this
    out.Header[1] = composited ? 1u : 0u;

    const AtmosphereSettings& s = m_atmosphere;
    SetRow(out.SunDir, s.sun.direction.Normalized(), s.sun.intensity);
    SetRow(out.SunColor, s.sun.color, s.sun.reddenGain);
    SetRow(out.MoonDir, s.moon.direction.Normalized(),
           s.moon.phase * 2.0f * std::numbers::pi_v<float>);
    SetRow(out.MoonTangent, s.moon.tangent.Normalized());
    SetRow(out.MoonColor, s.moon.color);
    SetRow(out.SkyColor, s.sky.color, s.sky.dayScale);
    SetRow(out.TintColor, s.sky.tintColor, s.sky.tintStrength);
    SetRow(out.NightColor, s.sky.nightColor, s.sky.dayWeight);
    out.SkyShape[0] = std::max(s.sky.tintExponent, EXPONENT_MIN);
    out.SkyShape[1] = s.sky.groundDensity;
    SetRow(out.GlareDir, s.glare.direction.Normalized(),
           std::clamp(s.glare.width, GLARE_WIDTH_MIN, GLARE_WIDTH_MAX));
    SetRow(out.GlareColor, s.glare.color);
    // Rodrigues terms of the turn onto the star sphere; no axis, no turn
    const Math::Vector3D axis = s.stars.axis.Normalized();
    const float turn = axis.Magnitude() > 0.0f ? s.stars.angle : 0.0f;
    SetRow(out.StarColor, s.stars.color, std::cos(turn));
    SetRow(out.StarAxis, axis, -std::sin(turn));
    out.FogDensity[0] = s.fog.densityPerMeter;
    out.FogDensity[1] = std::max(s.fog.exponent, EXPONENT_MIN);
    out.FogDensity[2] = s.fog.baseHeight;
    out.FogDensity[3] = s.fog.heightEnabled ? FOG_HEIGHT_FALLOFF : 0.0f;
    SetRow(out.FogIndoor, s.fog.indoorColor, s.fog.outdoor);
    out.FogBorder[0] = s.fog.dayWeight;
    out.FogBorder[1] = s.border.start;
    out.FogBorder[2] = s.border.end;
    // Camera medium; zero density leaves fog and sky as they are
    SetRow(out.Medium, s.medium.color,
           std::max(s.medium.densityPerMeter, 0.0f));
    out.MediumShape[0] = std::clamp(s.medium.veil, 0.0f, 1.0f);
    // Clouds once their noise exists (EnsureCloudNoise ran first)
    const AtmosphereSettings::CloudParams& c = s.clouds;
    if (m_cloudNoise && c.coverage > 0.0f) {
        const float coverage = std::min(c.coverage, 1.0f);
        out.CloudLayer[0] = c.baseHeight;
        out.CloudLayer[1] = CloudNoiseThreshold(coverage);
        out.CloudLayer[2] = c.wind.GetX();
        out.CloudLayer[3] = c.wind.GetY();
        SetRow(out.CloudLight, c.lightColor,
               std::clamp(c.lightFade, 0.0f, 1.0f));
        SetRow(out.CloudAmbient, c.ambientColor, coverage);
    }

    static bool warned = false;
    if (!warned && composited && s.fog.densityPerMeter > 0.0f &&
        m_frameData.LegacyFogEnabled()) {
        SLEAK_WARN(
            "Atmosphere: LightManager fog is on too; the scene is "
            "fogged twice");
        warned = true;
    }
}

void Renderer::EnsureCloudNoise() {
    if (m_cloudNoise || m_cloudNoiseFailed ||
        !(DrawsAtmosphere() || DrawsForwardSky()) ||
        m_atmosphere.clouds.coverage <= 0.0f)
        return;
    // Sampled with the defaults: linear, repeat, one level
    TextureDesc desc;
    desc.width = CLOUD_NOISE_SIZE;
    desc.height = CLOUD_NOISE_SIZE;
    desc.mipLevels = 1;
    const std::vector<uint8_t>& texels = CloudNoiseTexels();
    const TextureMipLevel level{texels.data(), texels.size()};
    m_cloudNoise = CreateTexture(desc, &level, 1);
    if (m_cloudNoise) return;
    SLEAK_ERROR("Atmosphere: cloud noise texture failed, no clouds");
    m_cloudNoiseFailed = true;
}

}  // namespace RenderEngine
}  // namespace Sleak
