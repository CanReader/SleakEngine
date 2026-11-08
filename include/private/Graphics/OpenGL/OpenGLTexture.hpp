#ifndef OPENGLTEXTURE_HPP_
#define OPENGLTEXTURE_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

/// OpenGL 2D texture, loadable from disk or an in-memory pixel buffer.
class ENGINE_API OpenGLTexture : public Texture {
public:
    OpenGLTexture();
    ~OpenGLTexture() override;

    /// Uploads raw pixel data as a new GL texture, generating mips.
    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    /// Uploads caller levels into immutable storage and builds missing ones.
    bool LoadMipChain(const TextureDesc& desc, const TextureMipLevel* levels,
                      uint32_t levelCount) override;
    /// Decodes an image file and uploads it as a new GL texture.
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;
    void SetLodBias(float bias) override;
    float GetLodBias() const override { return m_lodBias; }
    /// Forces nearest magnification; minification and mips keep SetFilter.
    void SetNearestMagnification(bool enabled) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::Texture2D; }
    /// True when the storage is GL_SRGB8_ALPHA8.
    bool IsSRGB() const override { return m_srgb; }

    uint64_t GetImGuiTextureID() const override { return static_cast<uint64_t>(m_texture); }

    GLuint GetGLTexture() const { return m_texture; }

private:
    GLuint m_texture = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
    bool m_srgb = false;
    TextureFilter m_filter = TextureFilter::Linear;
    float m_lodBias = 0.0f;
    bool m_nearestMagnification = false;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLTEXTURE_HPP_
