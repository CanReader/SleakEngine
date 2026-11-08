#include "../../include/private/Graphics/OpenGL/OpenGLTexture.hpp"
#include <Core/Logger.hpp>
#include <stb_image.h>

/// Bytes per texel of the RGBA8 family.
#define RGBA8_TEXEL_BYTES 4
/// Upper bound on stale GL errors drained before an upload.
#define GL_ERROR_DRAIN_LIMIT 64

namespace Sleak {
namespace RenderEngine {

namespace {

/// Drops stale GL errors so the next check reports only new ones.
void ClearGLErrors() {
    for (int i = 0; i < GL_ERROR_DRAIN_LIMIT; ++i)
        if (glGetError() == GL_NO_ERROR) return;
}

}  // namespace

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

/// Uploads caller levels into immutable storage and builds missing ones.
bool OpenGLTexture::LoadMipChain(const TextureDesc& desc,
                                 const TextureMipLevel* levels,
                                 uint32_t levelCount) {
    if (!levels || desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > GetFullMipCount(desc.width, desc.height) ||
        (levelCount != 1 && levelCount != desc.mipLevels)) {
        SLEAK_ERROR("OpenGLTexture: bad mip chain ({} of {} levels, {}x{})",
                    levels ? levelCount : 0u, desc.mipLevels, desc.width,
                    desc.height);
        return false;
    }
    for (uint32_t i = 0; i < levelCount; ++i) {
        const size_t levelBytes =
            static_cast<size_t>(GetMipExtent(desc.width, i)) *
            GetMipExtent(desc.height, i) * RGBA8_TEXEL_BYTES;
        if (!levels[i].pixels || levels[i].sizeBytes != levelBytes) {
            SLEAK_ERROR("OpenGLTexture: mip {} has {} bytes, expected {}", i,
                        levels[i].pixels ? levels[i].sizeBytes : 0, levelBytes);
            return false;
        }
    }

    if (m_texture != 0) {
        SLEAK_ERROR(
            "OpenGLTexture: texture {} already has contents; create a new "
            "texture instead",
            m_texture);
        return false;
    }
    m_width = desc.width;
    m_height = desc.height;
    m_format = desc.format;
    m_srgb = desc.srgb;

    const GLenum internalFormat = desc.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    const GLenum pixelFormat =
        desc.format == TextureFormat::BGRA8 ? GL_BGRA : GL_RGBA;
    ClearGLErrors();

    glCreateTextures(GL_TEXTURE_2D, 1, &m_texture);
    glTextureStorage2D(m_texture, static_cast<GLsizei>(desc.mipLevels),
                       internalFormat, static_cast<GLsizei>(desc.width),
                       static_cast<GLsizei>(desc.height));
    for (uint32_t i = 0; i < levelCount; ++i)
        glTextureSubImage2D(m_texture, static_cast<GLint>(i), 0, 0,
                            static_cast<GLsizei>(GetMipExtent(desc.width, i)),
                            static_cast<GLsizei>(GetMipExtent(desc.height, i)),
                            pixelFormat, GL_UNSIGNED_BYTE, levels[i].pixels);
    if (levelCount < desc.mipLevels) glGenerateTextureMipmap(m_texture);

    // Same sampling defaults as LoadFromMemory
    glTextureParameteri(m_texture, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_texture, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_texture, GL_TEXTURE_WRAP_T, GL_REPEAT);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SLEAK_ERROR("OpenGLTexture: mip chain upload failed (GL error 0x{:X})",
                    error);
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
        return false;
    }
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
    if (m_nearestMagnification)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/// Forces nearest magnification; minification and mips keep SetFilter.
void OpenGLTexture::SetNearestMagnification(bool enabled) {
    if (enabled == m_nearestMagnification) return;
    m_nearestMagnification = enabled;
    if (m_texture == 0) return;
    const bool nearest = enabled || m_filter == TextureFilter::Nearest;
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    nearest ? GL_NEAREST : GL_LINEAR);
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
