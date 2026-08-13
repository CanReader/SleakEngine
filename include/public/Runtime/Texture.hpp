#ifndef _TEXTURE_H_
#define _TEXTURE_H_

#include <string>
#include <cstdint>

namespace Sleak {
/// Pixel layout a Texture's data is stored/uploaded in.
enum class TextureFormat {
    RGBA8,
    RGB8,
    BGRA8,
    DXT1,
    DXT5,
};

/// Dimensionality/layout a Texture represents on the GPU.
enum class TextureType {
    Texture2D,
    TextureCube,
    Texture3D,
};

/// Sampling quality, from point sampling up through anisotropic filtering.
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
enum class TextureWrapMode {
    Repeat,
    ClampToEdge,
    ClampToBorder,
    Mirror,
    MirrorClampToEdge,
};

/// Backend-agnostic GPU texture interface; each renderer backend supplies its own implementation.
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

    virtual uint64_t GetImGuiTextureID() const { return 0; }
};

} // namespace Sleak

#endif