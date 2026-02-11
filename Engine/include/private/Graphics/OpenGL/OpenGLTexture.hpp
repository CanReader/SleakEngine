#ifndef OPENGLTEXTURE_HPP_
#define OPENGLTEXTURE_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API OpenGLTexture : public Texture {
public:
    OpenGLTexture();
    ~OpenGLTexture() override;

    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::Texture2D; }

    GLuint GetGLTexture() const { return m_texture; }

private:
    GLuint m_texture = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLTEXTURE_HPP_
