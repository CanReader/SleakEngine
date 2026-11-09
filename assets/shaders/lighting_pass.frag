#version 450
#extension GL_GOOGLE_include_directive : require

// Deferred lighting pass, Vulkan engine default
#include "sleak/lighting_default.glsl"

void main() { SleakLightingDefaultMain(); }
