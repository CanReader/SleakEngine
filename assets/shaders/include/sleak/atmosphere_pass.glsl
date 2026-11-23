#ifndef SLEAK_ATMOSPHERE_PASS_GLSL
#define SLEAK_ATMOSPHERE_PASS_GLSL

#include "sleak/atmosphere.glsl"

// Screen-space entry points of the atmosphere: the Vulkan composite pass
// and the OpenGL lighting stems. uv is the fullscreen triangle's fragUV.

// Engine NDC depth of the far plane, where sky rays are built
#define SLEAK_ATMO_FAR_NDC_Z 1.0
// Pixels this close to fog-free keep their color (no blend write)
#define SLEAK_ATMO_SKIP_TRANSMITTANCE 0.999
// Transmittance of a sky pixel: the sky replaces it
#define SLEAK_ATMO_SKY_TRANSMITTANCE 0.0

/// This frame's TAA jitter in engine NDC (y up); FrameData stores it after
/// the Vulkan Y flip.
vec2 SleakFrameJitterNdc() {
#ifdef VULKAN
    return vec2(uFrame.Jitter.x, -uFrame.Jitter.y);
#else
    return uFrame.Jitter.xy;
#endif
}

/// Camera-relative world position of a geometry pixel, jitter removed.
vec3 SleakAtmosphereViewRel(vec2 uv, float depth) {
    vec2 ndc = SleakUvToNdc(uv) - SleakFrameJitterNdc();
    vec4 p = uFrame.InvViewProjRel * vec4(ndc, SleakDepthToNdc(depth), 1.0);
    return p.xyz / p.w;
}

/// World direction through a pixel. Not jittered: TAA passes all-sky
/// neighbourhoods through without history (phase 14 changes both).
vec3 SleakAtmosphereViewDir(vec2 uv) {
    vec4 p = uFrame.InvViewProjRel *
             vec4(SleakUvToNdc(uv), SLEAK_ATMO_FAR_NDC_Z, 1.0);
    return normalize(p.xyz / p.w);
}

/// Inscatter and transmittance between the camera and a world position.
vec4 SleakAtmosphereScatterWorld(vec3 worldPos) {
    return SleakAtmosphereScatter(worldPos - uFrame.CameraPos.xyz,
                                  worldPos.y);
}

/// rgb inscatter, a transmittance for one pixel at its final depth.
vec4 SleakAtmosphereComposite(vec2 uv, float depth) {
    if (depth >= 1.0)
        return vec4(SleakAtmosphereSky(SleakAtmosphereViewDir(uv)),
                    SLEAK_ATMO_SKY_TRANSMITTANCE);
    vec3 rel = SleakAtmosphereViewRel(uv, depth);
    return SleakAtmosphereScatter(rel, rel.y + uFrame.CameraPos.y);
}

/// A scene color seen through the atmosphere.
vec3 SleakAtmosphereApply(vec3 scene, vec4 scatter) {
    return scene * scatter.a + scatter.rgb;
}

/// Magenta over everything while SleakAtmosphereStale holds.
vec4 SleakAtmosphereGuard(vec4 scatter) {
    return SleakAtmosphereStale()
               ? vec4(SLEAK_FRAME_GUARD_COLOR.rgb,
                      SLEAK_ATMO_SKY_TRANSMITTANCE)
               : scatter;
}

#endif
