#ifndef _GRAPHICSCONFIG_HPP_
#define _GRAPHICSCONFIG_HPP_

#include <Core/OSDef.hpp>
#include <cstdint>

namespace Sleak {

class DirectionalLight;

enum class GraphicsQuality { Off, Low, Medium, High, Ultra };

struct ENGINE_API GraphicsConfig {
    // Post-FX (deferred path)
    bool  ssaoEnabled = false;
    float ssaoRadius  = 0.5f;
    float ssaoBias    = 0.012f;
    float ssaoPower   = 2.5f;
    bool  ssrEnabled  = false;
    bool  bloomEnabled = true;
    bool  iblEnabled   = false;
    float iblIntensity = 1.0f;
    bool  taaEnabled   = false;

    // Shadows
    bool     shadowEnabled       = true;
    uint32_t shadowMapResolution = 2048;
    float    shadowDistance      = 160.0f;
    float    shadowFrustumSize   = 96.0f;
    float    shadowCasterDistance = 96.0f;  // game uses for shadow-caster culling
    float    shadowBias          = 0.002f;
    float    shadowStrength      = 1.0f;

    // Culling
    bool     frustumCullingEnabled   = true;
    bool     occlusionCullingEnabled = true;
    uint32_t occlusionBufferWidth    = 256;
    uint32_t occlusionBufferHeight   = 144;
    uint32_t maxOccluders            = 192;

    // Misc
    uint32_t msaaSamples = 1;

    // Factory: build a config from a named quality preset.
    static GraphicsConfig Preset(GraphicsQuality q);

    // Apply the shadow-related fields onto a directional light.
    void ApplyShadows(Sleak::DirectionalLight& light) const;
};

}  // namespace Sleak

#endif  // _GRAPHICSCONFIG_HPP_
