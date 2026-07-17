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
    float skyDensityDay        = 0.35f;
    float skyDensityNight      = 0.65f;
    float skyDensityWeather    = 1.5f;
    float skyHorizonNear       = 1.0f;
    float skyHorizonFar        = 1.5f;
    bool  sunMoonDiscEnabled   = false;
    bool  starsEnabled         = false;

    // Time-of-day inputs — game writes per frame, engine only consumes
    float    timeAngle      = 0.25f;
    float    timeBrightness = 1.0f;
    float    sunVisibility  = 1.0f;
    float    shadowFade     = 1.0f;
    float    rainStrength   = 0.0f;
    uint32_t moonPhase      = 0;

    // Volumetric light shafts
    bool     lightShaftEnabled  = false;
    float    lightShaftStrength = 1.0f;
    uint32_t lightShaftSamples  = 7;
    float    lightShaftMaxDist  = 128.0f;
    float    lightShaftMorning  = 0.25f;
    float    lightShaftDay      = 0.10f;
    float    lightShaftNight    = 0.50f;
    float    lightShaftWeather  = 8.0f;

    // Clouds
    uint32_t cloudMode       = 0;  // 0=off 1=2D 2=volumetric
    uint32_t cloudBase       = 0;  // 0=perlin 1=worley 2=blocky
    uint32_t cloudSamples    = 32;
    float    cloudHeight     = 192.0f;
    float    cloudThickness  = 5.0f;
    float    cloudAmount     = 10.0f;
    float    cloudDensity    = 4.0f;
    float    cloudSpeed      = 1.0f;
    float    cloudBrightness = 1.0f;
    float    cloudOpacity    = 1.0f;

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
    float blocklightR         = 1.0f;
    float blocklightG         = 1.0f;
    float blocklightB         = 1.0f;
    float minLight            = 0.0f;
    bool  desaturationEnabled = false;
    float desaturationFactor  = 1.5f;

    // Waving
    bool  wavingEnabled  = false;
    float wavingSpeed    = 1.0f;
    float wavingStrength = 1.0f;

    // Render scale
    float renderScale = 1.0f;

    // Factory: build a config from a named quality preset.
    static GraphicsConfig Preset(GraphicsQuality q);

    // Apply the shadow-related fields onto a directional light.
    void ApplyShadows(Sleak::DirectionalLight& light) const;
};

}  // namespace Sleak

#endif  // _GRAPHICSCONFIG_HPP_
