#version 450
#extension GL_GOOGLE_include_directive : require

// Atmosphere composite (Vulkan): sky and fog over the HDR scene

#include "sleak/lighting_inputs.glsl"
#include "sleak/atmosphere_pass.glsl"

void main() {
    float depth = texelFetch(gDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 scatter = SleakAtmosphereComposite(fragUV, depth);
    if (depth < 1.0 && scatter.a >= SLEAK_ATMO_SKIP_TRANSMITTANCE) discard;
    outColor = SleakAtmosphereGuard(scatter);
}
