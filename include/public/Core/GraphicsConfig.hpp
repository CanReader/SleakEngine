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

    // Tonemap / post
    bool  tonemapEnabled = false;
    float exposure       = 1.0f;
    float gamma          = 2.2f;

    // PCSS soft shadows
    bool pcssEnabled = true;

    // Procedural sky
    bool  skyProceduralEnabled = false;
    float skyHorizonNear       = 1.0f;
    float skyHorizonFar        = 1.5f;

    // Volumetric light shafts
    bool     lightShaftEnabled  = false;
    float    lightShaftStrength = 1.0f;
    uint32_t lightShaftSamples  = 7;
    float    lightShaftMaxDist  = 128.0f;

    // AA / sharpen
    bool  fxaaEnabled     = false;
    float fxaaSubpixel    = 0.5f;
    float sharpenStrength = 0.0f;

    // Grading
    float saturation          = 1.0f;
    float vibrance            = 1.0f;
    bool  vignetteEnabled     = false;
    float vignetteStrength    = 1.06f;
    bool  filmGrainEnabled    = false;
    bool  autoExposureEnabled = false;
    float autoExposureRadius  = 0.7f;
    float autoExposureSpeed   = 3.33f;

    // Lighting
    float minLight            = 0.0f;
    bool  desaturationEnabled = false;
    float desaturationFactor  = 1.5f;

    // Render scale
    float renderScale = 1.0f;

    // Factory: build a config from a named quality preset.
    static GraphicsConfig Preset(GraphicsQuality q);

    // Apply the shadow-related fields onto a directional light.
    void ApplyShadows(Sleak::DirectionalLight& light) const;
};

}  // namespace Sleak

#endif  // _GRAPHICSCONFIG_HPP_
