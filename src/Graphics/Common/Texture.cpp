#include <Core/Logger.hpp>
#include <Runtime/Texture.hpp>

#include "../../include/private/Graphics/Common/ResourceManager.hpp"

#define TEXTURE_MAX_EXTENT 16384
#define RGBA8_TEXEL_BYTES 4

namespace Sleak {

/// Validates the request, then hands it to the active backend.
RefPtr<Texture> CreateTexture(const TextureDesc& desc,
                              const TextureMipLevel* levels,
                              uint32_t levelCount) {
    // Size and format
    if (desc.width == 0 || desc.height == 0 ||
        desc.width > TEXTURE_MAX_EXTENT || desc.height > TEXTURE_MAX_EXTENT) {
        SLEAK_ERROR("CreateTexture: size {}x{} outside 1..{}", desc.width,
                    desc.height, TEXTURE_MAX_EXTENT);
        return RefPtr<Texture>();
    }
    if (desc.format != TextureFormat::RGBA8 &&
        desc.format != TextureFormat::BGRA8) {
        SLEAK_ERROR("CreateTexture: format {} unsupported, use RGBA8 or BGRA8",
                    static_cast<int>(desc.format));
        return RefPtr<Texture>();
    }

    // Level count
    const uint32_t fullCount = GetFullMipCount(desc.width, desc.height);
    const uint32_t mipCount = desc.mipLevels == 0 ? fullCount : desc.mipLevels;
    if (mipCount > fullCount) {
        SLEAK_ERROR(
            "CreateTexture: {} mip levels requested, a {}x{} image has {}",
            mipCount, desc.width, desc.height, fullCount);
        return RefPtr<Texture>();
    }
    if (!levels || (levelCount != 1 && levelCount != mipCount)) {
        SLEAK_ERROR(
            "CreateTexture: pass level 0 alone or all {} levels (got {})",
            mipCount, levels ? levelCount : 0u);
        return RefPtr<Texture>();
    }

    // Every supplied level's pixels and byte size
    for (uint32_t i = 0; i < levelCount; ++i) {
        const uint32_t w = GetMipExtent(desc.width, i);
        const uint32_t h = GetMipExtent(desc.height, i);
        const size_t expected = static_cast<size_t>(w) * h * RGBA8_TEXEL_BYTES;
        if (!levels[i].pixels || levels[i].sizeBytes != expected) {
            SLEAK_ERROR(
                "CreateTexture: level {} is {} bytes, expected {} for {}x{}", i,
                levels[i].pixels ? levels[i].sizeBytes : 0, expected, w, h);
            return RefPtr<Texture>();
        }
    }

    TextureDesc resolved = desc;
    resolved.mipLevels = mipCount;
    return RefPtr<Texture>(RenderEngine::ResourceManager::CreateTextureFromDesc(
        resolved, levels, levelCount));
}

}  // namespace Sleak
