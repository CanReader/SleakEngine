#ifndef _TEXTURE_H_
#define _TEXTURE_H_

#include <string>
#include <cstdint>

namespace Sleak {
enum class TextureFormat {
    RGBA8,
    RGB8,
    BGRA8,
    DXT1,
    DXT5,
};

enum class TextureType {
    Texture2D,
    TextureCube,
    Texture3D,
};

enum class TextureFilter {
    Nearest,
    Linear,
    Anisotropic,
};

enum class TextureWrapMode {
    Repeat,
    ClampToEdge,
    ClampToBorder,
    Mirror,
    MirrorClampToEdge,
};

class Texture { 
public:
    virtual ~Texture() = default;

    virtual bool LoadFromMemory(const void* data, uint32_t width, uint32_t height, TextureFormat format) = 0;

    virtual bool LoadFromFile(const std::string& filePath) = 0;

    virtual void Bind(uint32_t slot = 0) const = 0;

    virtual void Unbind() const = 0;

    virtual void SetFilter(TextureFilter filter) = 0;

    virtual void SetWrapMode(TextureWrapMode wrapMode) = 0;

    virtual uint32_t GetWidth() const = 0;

    virtual uint32_t GetHeight() const = 0;

    virtual TextureFormat GetFormat() const = 0;

    virtual TextureType GetType() const = 0;
};

} // namespace Sleak

#endif