#ifndef _FRAME_DATA_BUILDER_HPP_
#define _FRAME_DATA_BUILDER_HPP_

#include <array>
#include <cstdint>

#include "ConstantBuffer.hpp"

namespace Sleak {
namespace RenderEngine {

/// What a backend knows about this frame that the engine core does not.
struct FrameTargetInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    float jitter[2] = {};  // NDC after the backend's Y flip
    uint32_t featureFlags = 0;
    uint32_t cascadeCount = 0;
};

/// Near and far plane of the active scene camera, or the engine fallbacks.
void GetMainCameraPlanes(float& nearPlane, float& farPlane);

/// Builds FrameData once per frame from the engine clock, the main camera and
/// the light LightManager sent, and holds the game's opaque block.
class FrameDataBuilder {
   public:
    /// Records this frame's primary light.
    void SetLight(const ShadowLightUBO& light);
    /// Copies the game block and zero-fills the rest; false when too large.
    bool SetUserData(const void* data, uint32_t size);
    /// Fills out for this frame and keeps it as the previous frame.
    void Build(const FrameTargetInfo& target, FrameData& out);
    /// The game block, always FRAME_USER_DATA_MAX_BYTES long.
    const uint8_t* GetUserData() const { return m_userData.data(); }

   private:
    std::array<uint8_t, FRAME_USER_DATA_MAX_BYTES> m_userData{};
    float m_lightDir[4] = {};
    float m_lightColor[4] = {};
    float m_ambient[4] = {};
    double m_prevViewProj[16] = {};
    double m_prevCameraPos[3] = {};
    float m_prevJitter[2] = {};
    bool m_hasPrevious = false;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif
