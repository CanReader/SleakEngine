#include "../../include/private/Graphics/OpenGL/OpenGLTexture.hpp"
#include <Logger.hpp>
#include <stb_image.h>

namespace Sleak {
namespace RenderEngine {

OpenGLTexture::OpenGLTexture() {}

OpenGLTexture::~OpenGLTexture() {
    if (m_texture != 0) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
}

bool OpenGLTexture::LoadFromMemory(const void* data, uint32_t width,
                                    uint32_t height, TextureFormat format) {
    m_width = width;
    m_height = height;
    m_format = format;

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);

    GLenum glFormat = GL_RGBA;
    if (format == TextureFormat::RGB8) glFormat = GL_RGB;

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, glFormat,
                 GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Default: trilinear (good quality baseline)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool OpenGLTexture::LoadFromFile(const std::string& filePath) {
    int w, h, channels;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* pixels = stbi_load(filePath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        SLEAK_ERROR("OpenGLTexture: Failed to load image: {}",
                    filePath);
        return false;
    }

    bool result = LoadFromMemory(pixels, static_cast<uint32_t>(w),
                                 static_cast<uint32_t>(h),
                                 TextureFormat::RGBA8);
    stbi_image_free(pixels);
    return result;
}

void OpenGLTexture::Bind(uint32_t slot) const {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_texture);
}

void OpenGLTexture::Unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLTexture::SetFilter(TextureFilter filter) {
    m_filter = filter;
    glBindTexture(GL_TEXTURE_2D, m_texture);
    switch (filter) {
        case TextureFilter::Nearest:
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            break;
        case TextureFilter::Bilinear:
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case TextureFilter::Trilinear:
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case TextureFilter::Anisotropic2x:
        case TextureFilter::Anisotropic4x:
        case TextureFilter::Anisotropic8x:
        case TextureFilter::Anisotropic16x: {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            float aniso = 1.0f;
            if (filter == TextureFilter::Anisotropic2x)  aniso = 2.0f;
            else if (filter == TextureFilter::Anisotropic4x)  aniso = 4.0f;
            else if (filter == TextureFilter::Anisotropic8x)  aniso = 8.0f;
            else if (filter == TextureFilter::Anisotropic16x) aniso = 16.0f;
            float maxSupported;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxSupported);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                            aniso < maxSupported ? aniso : maxSupported);
            break;
        }
        default:
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLTexture::SetLodBias(float bias) {
    m_lodBias = bias;
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLTexture::SetWrapMode(TextureWrapMode wrapMode) {
    glBindTexture(GL_TEXTURE_2D, m_texture);
    GLenum mode = GL_REPEAT;
    switch (wrapMode) {
        case TextureWrapMode::ClampToEdge:
            mode = GL_CLAMP_TO_EDGE; break;
        case TextureWrapMode::ClampToBorder:
            mode = GL_CLAMP_TO_BORDER; break;
        case TextureWrapMode::Mirror:
            mode = GL_MIRRORED_REPEAT; break;
        default:
            mode = GL_REPEAT; break;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace RenderEngine
}  // namespace Sleak
