#ifndef _ATMOSPHERE_HPP_
#define _ATMOSPHERE_HPP_

#include <Math/Vector.hpp>

namespace Sleak {

/// Procedural sky, sun, moon, stars and fog of one frame, set through
/// Application::SetAtmosphere. Radiance is in linear scene units, directions
/// are unit world vectors; the defaults draw a black sky and no fog.
/// @ingroup rendering
struct AtmosphereSettings {
    /// Day gradient with its sun-side tint, and the night sky; fog takes
    /// its color from the same values.
    struct SkyParams {
        Math::Vector3D color;        // day base before the soft clip
        float dayScale = 0.0f;       // day brightness after the soft clip
        float groundDensity = 0.0f;  // darkening below the horizon
        Math::Vector3D tintColor;    // sun-side tint radiance
        float tintStrength = 0.0f;   // peak share of the tint
        float tintExponent = 1.0f;   // larger keeps the tint near the sun
        Math::Vector3D nightColor;   // night sky at the horizon
        float dayWeight = 1.0f;      // 0 night sky, 1 day sky
    };
    /// Sun disc and small halo, reddened toward the horizon.
    struct SunParams {
        Math::Vector3D direction;  // toward the sun
        Math::Vector3D color;      // base raised to the redden power
        float intensity = 0.0f;    // radiance scale, 0 hides the sun
        float reddenGain = 0.0f;   // extra power of the color at the horizon
    };
    /// Moon disc with phase shading and small halo.
    struct MoonParams {
        Math::Vector3D direction;  // toward the moon
        Math::Vector3D tangent;    // direction of travel, the phase axis
        Math::Vector3D color;      // radiance of the lit disc, black hides it
        float phase = 0.0f;        // lunation fraction, 0 full, 0.5 new
    };
    /// Star field fixed to a sphere that turns with the sky.
    struct StarParams {
        Math::Vector3D color;  // brightest star, black hides the stars
        Math::Vector3D axis;   // axis the sky turns around
        float angle = 0.0f;    // radians the sky has turned
    };
    /// Halo around the sun or moon on sky pixels.
    struct GlareParams {
        Math::Vector3D direction;  // toward the light
        Math::Vector3D color;      // radiance at the lobe peak, black is off
        float width = 0.0f;        // lobe width in (0, 1)
    };
    /// Power-law height fog toward a sky-matched color, off at zero density.
    struct FogParams {
        float densityPerMeter = 0.0f;
        float exponent = 1.0f;       // T = exp(-strength depth^exponent)
        bool heightEnabled = false;  // thins above baseHeight
        float baseHeight = 0.0f;     // world y where the thinning starts
        float dayWeight = 1.0f;      // 0 night fog color, 1 day color
        Math::Vector3D indoorColor;  // fog color with no sky at the eye
        float outdoor = 1.0f;        // 1 open sky at the camera
    };
    /// Fade into the sky gradient at the edge of the drawn world.
    struct BorderParams {
        float start = 0.0f;  // horizontal distance where the fade begins
        float end = 0.0f;    // full sky; end <= start turns it off
    };
    /// Soft cloud layer in a 60 m slab above baseHeight, drifting with the
    /// wind and lit from glare.direction. Off at zero coverage.
    struct CloudParams {
        float coverage = 0.0f;        // sky share at the layer's core, 0 off
        float baseHeight = 0.0f;      // world y of the flat base
        Math::Vector2D wind;          // drift in m/s along world x and z
        Math::Vector3D lightColor;    // direct light on the clouds
        Math::Vector3D ambientColor;  // sky light on the clouds
        float lightFade = 1.0f;       // 0 while the light flips sun to moon
    };

    SkyParams sky;
    SunParams sun;
    MoonParams moon;
    StarParams stars;
    GlareParams glare;
    FogParams fog;
    BorderParams border;
    CloudParams clouds;
};

}  // namespace Sleak

#endif  // _ATMOSPHERE_HPP_
