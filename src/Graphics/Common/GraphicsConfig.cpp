#include <Core/GraphicsConfig.hpp>

#include <Lighting/DirectionalLight.hpp>

namespace Sleak {

/// Builds a config with every render feature toggle set for a named quality tier.
GraphicsConfig GraphicsConfig::Preset(GraphicsQuality q) {
    GraphicsConfig cfg;
    switch (q) {
        case GraphicsQuality::Off: {
            cfg.ssaoEnabled = false;
            cfg.ssrEnabled = false;
            cfg.bloomEnabled = false;
            cfg.iblEnabled = false;
            cfg.taaEnabled = false;
            cfg.lightShaftEnabled = false;
            cfg.shadowEnabled = false;
            cfg.pcssEnabled = false;
            cfg.shadowCascadeCount = 1;
            cfg.occlusionCullingEnabled = false;
            cfg.occlusionBufferWidth = 160;
            cfg.occlusionBufferHeight = 90;
            break;
        }
        case GraphicsQuality::Low: {
            // Shadows only — no filter, no post-FX.
            cfg.ssaoEnabled = false;
            cfg.ssrEnabled = false;
            cfg.bloomEnabled = false;
            cfg.iblEnabled = false;
            cfg.taaEnabled = false;
            cfg.lightShaftEnabled = false;
            cfg.shadowEnabled = true;
            cfg.pcssEnabled = false;
            cfg.shadowMapResolution = 1024;
            cfg.shadowFrustumSize = 96.0f;
            cfg.shadowCascadeCount = 2;
            cfg.shadowSplitLambda = 0.5f;
            cfg.shadowMaxDistance = 96.0f;
            cfg.shadowCasterDistance = 64.0f;
            cfg.occlusionCullingEnabled = false;
            cfg.occlusionBufferWidth = 160;
            cfg.occlusionBufferHeight = 90;
            break;
        }
        case GraphicsQuality::Medium: {
            // Shadows + shadow filter + SSAO.
            cfg.ssaoEnabled = true;
            cfg.ssrEnabled = false;
            cfg.bloomEnabled = false;
            cfg.iblEnabled = false;
            cfg.taaEnabled = false;
            cfg.lightShaftEnabled = false;
            cfg.shadowEnabled = true;
            cfg.pcssEnabled = true;
            cfg.shadowCascadeCount = 3;
            cfg.shadowSplitLambda = 0.6f;
            cfg.shadowMaxDistance = 128.0f;
            cfg.occlusionCullingEnabled = true;
            cfg.occlusionBufferWidth = 256;
            cfg.occlusionBufferHeight = 144;
            break;
        }
        case GraphicsQuality::High: {
            // Shadows + SSAO + SSR + bloom + IBL + light shafts.
            cfg.ssaoEnabled = true;
            cfg.ssrEnabled = true;
            cfg.bloomEnabled = true;
            cfg.iblEnabled = true;
            cfg.taaEnabled = false;
            cfg.lightShaftEnabled = true;
            cfg.shadowEnabled = true;
            cfg.pcssEnabled = true;
            cfg.shadowCascadeCount = 4;
            cfg.shadowSplitLambda = 0.7f;
            cfg.shadowMaxDistance = 160.0f;
            cfg.occlusionCullingEnabled = true;
            cfg.occlusionBufferWidth = 256;
            cfg.occlusionBufferHeight = 144;
            break;
        }
        case GraphicsQuality::Ultra: {
            // High + more shadow range in the same cascades + TAA.
            cfg.ssaoEnabled = true;
            cfg.ssrEnabled = true;
            cfg.bloomEnabled = true;
            cfg.iblEnabled = true;
            cfg.taaEnabled = true;
            cfg.lightShaftEnabled = true;
            cfg.shadowEnabled = true;
            cfg.pcssEnabled = true;
            cfg.shadowMapResolution = 3072;
            cfg.shadowDistance = 256.0f;
            cfg.shadowCascadeCount = 4;
            cfg.shadowSplitLambda = 0.75f;
            cfg.shadowMaxDistance = 256.0f;
            cfg.shadowCasterDistance = 256.0f;
            cfg.occlusionCullingEnabled = true;
            cfg.occlusionBufferWidth = 320;
            cfg.occlusionBufferHeight = 180;
            cfg.maxOccluders = 256;
            break;
        }
    }
    return cfg;
}

/// Pushes this config's shadow settings onto a directional light.
void GraphicsConfig::ApplyShadows(Sleak::DirectionalLight& light) const {
    light.SetCastShadows(shadowEnabled);
    light.SetShadowFrustumSize(shadowFrustumSize);
    light.SetShadowDistance(shadowDistance);
    light.SetShadowBias(shadowBias);
    light.SetShadowStrength(shadowStrength);
    light.SetShadowCascadeCount(shadowCascadeCount);
    light.SetShadowSplitLambda(shadowSplitLambda);
    light.SetShadowMaxDistance(shadowMaxDistance);
}

}  // namespace Sleak
