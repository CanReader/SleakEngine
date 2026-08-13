#ifndef OPENGLCUBEMAPTEXTURE_HPP_
#define OPENGLCUBEMAPTEXTURE_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <glad/glad.h>
#include <array>
#include <string>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API OpenGLCubemapTexture : public ::Sleak::Texture {
public:
    OpenGLCubemapTexture();
    ~OpenGLCubemapTexture() override;

    /// Load 6 face images: +X, -X, +Y, -Y, +Z, -Z
    bool LoadCubemap(const std::array<std::string, 6>& facePaths);

    /// Load from a single equirectangular panorama and convert to cubemap
    bool LoadEquirectangular(const std::string& path, uint32_t faceSize = 512);

    /// Generate a procedural gradient cubemap (top/mid/bottom colors)
    bool LoadGradient(float topR, float topG, float topB,
                      float midR, float midG, float midB,
                      float botR, float botG, float botB,
                      uint32_t resolution = 64);

    // Texture interface — LoadFromFile/LoadFromMemory not used for cubemaps
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
    TextureType GetType() const override { return TextureType::TextureCube; }

    GLuint GetGLTexture() const { return m_texture; }

private:
    GLuint m_texture = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGB8;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLCUBEMAPTEXTURE_HPP_
