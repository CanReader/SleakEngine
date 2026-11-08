#ifndef _TEXTURE_H_
#define _TEXTURE_H_

#include <Core/OSDef.hpp>
#include <Memory/RefPtr.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Sleak {
/// Pixel layout a Texture's data is stored/uploaded in.
/// @ingroup rendering
enum class TextureFormat {
    RGBA8,
    RGB8,
    BGRA8,
    DXT1,
    DXT5,
};

/// Dimensionality/layout a Texture represents on the GPU.
/// @ingroup rendering
enum class TextureType {
    Texture2D,
    TextureCube,
    Texture3D,
};

/// Sampling quality, from point sampling up through anisotropic filtering.
/// @ingroup rendering
enum class TextureFilter {
    Nearest,        // Point / no filtering
    Bilinear,       // Linear min/mag, nearest mip
    Trilinear,      // Linear min/mag/mip (best quality without anisotropy)
    Anisotropic2x,  // Anisotropic 2x
    Anisotropic4x,  // Anisotropic 4x
    Anisotropic8x,  // Anisotropic 8x
    Anisotropic16x, // Anisotropic 16x (highest quality)
    // Legacy aliases kept for backward compatibility
    Linear      = Trilinear,
    Anisotropic = Anisotropic16x,
};

/// How UV coordinates outside [0,1] are resolved when sampling.
/// @ingroup rendering
enum class TextureWrapMode {
    Repeat,
    ClampToEdge,
    ClampToBorder,
    Mirror,
    MirrorClampToEdge,
};

/// Size, pixel format, color space and mip count of a texture built from
/// caller pixels with CreateTexture().
/// @ingroup rendering
struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    /// RGBA8 or BGRA8; other formats are rejected.
    TextureFormat format = TextureFormat::RGBA8;
    /// Store as sRGB so sampling returns linear color.
    bool srgb = false;
    /// Total levels; 0 means the full chain down to 1x1.
    uint32_t mipLevels = 1;
};

/// Pixels of one mip level, rows top to bottom with no row padding.
/// @ingroup rendering
struct TextureMipLevel {
    const void* pixels = nullptr;
    size_t sizeBytes = 0;
};

/// Level count of a full mip chain for a width x height image.
inline uint32_t GetFullMipCount(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    for (uint32_t size = width > height ? width : height; size > 1; size >>= 1)
        ++levels;
    return levels;
}

/// Edge length of a mip level, never below one texel.
inline uint32_t GetMipExtent(uint32_t size, uint32_t level) {
    for (uint32_t i = 0; i < level && size > 1; ++i) size >>= 1;
    return size > 0 ? size : 1;
}

/// Backend-agnostic GPU texture interface; each renderer backend supplies its
/// own implementation.
///
/// You rarely construct one directly. Materials create and own their
/// textures when you call a `Set...Texture(path)` setter, ModelLoader
/// creates them while importing, and Sleak::CreateTexture makes one from
/// caller pixels with an explicit format, sRGB flag and mip chain. The
/// concrete type is whatever the active backend provides, which is why
/// every method here is virtual.
///
/// Call Sleak::CreateTexture on the thread that drives the renderer. It
/// copies the pixels before returning and takes either level 0 alone or
/// every level of the resolved chain.
///
/// Filtering and wrap mode are set per texture, not per material, so
/// change them on the object a material hands back. Reach for
/// TextureFilter::Nearest on pixel art and atlases where bleeding between
/// neighboring tiles would show, and an anisotropic mode on ground planes
/// viewed at glancing angles.
///
/// @code{.cpp}
/// // Load through a material, then adjust sampling on the result
/// material->SetDiffuseTexture("assets/textures/atlas.png");
///
/// if (Sleak::Texture* tex = material->GetDiffuseTexture()) {
///     tex->SetFilter(Sleak::TextureFilter::Nearest);
///     tex->SetWrapMode(Sleak::TextureWrapMode::ClampToEdge);
///     SLEAK_INFO("Atlas is {}x{}", tex->GetWidth(), tex->GetHeight());
/// }
/// @endcode
///
/// @see Material, TextureFilter, TextureWrapMode, TextureFormat, ModelLoader
/// @ingroup rendering
class Texture {
public:
    virtual ~Texture() = default;

    /// Uploads raw pixel data as the texture's contents, replacing any existing image.
    virtual bool LoadFromMemory(const void* data, uint32_t width, uint32_t height, TextureFormat format) = 0;

    /// Loads and uploads an image file from disk.
    virtual bool LoadFromFile(const std::string& filePath) = 0;

    virtual void Bind(uint32_t slot = 0) const = 0;

    virtual void Unbind() const = 0;

    virtual void SetFilter(TextureFilter filter) = 0;

    virtual void SetWrapMode(TextureWrapMode wrapMode) = 0;

    virtual uint32_t GetWidth() const = 0;

    virtual uint32_t GetHeight() const = 0;

    virtual TextureFormat GetFormat() const = 0;

    virtual TextureType GetType() const = 0;

    virtual void SetLodBias(float bias) {}
    virtual float GetLodBias() const { return 0.0f; }

    /// Forces nearest magnification; minification and mips keep SetFilter.
    virtual void SetNearestMagnification(bool enabled) {}

    /// True when the GPU decodes this texture from sRGB when sampling.
    virtual bool IsSRGB() const { return false; }

    virtual uint64_t GetImGuiTextureID() const { return 0; }
};

/// Creates a 2D texture from level 0 alone (the backend builds the other
/// mips) or from every level; returns a null RefPtr on bad input or failure.
ENGINE_API RefPtr<Texture> CreateTexture(const TextureDesc& desc,
                                         const TextureMipLevel* levels,
                                         uint32_t levelCount);

} // namespace Sleak

#endif