#version 450 core
#extension GL_GOOGLE_include_directive : require

// Deferred lighting pass, OpenGL engine default
#include "sleak/lighting_default.glsl"

void main() { SleakLightingDefaultMain(); }
