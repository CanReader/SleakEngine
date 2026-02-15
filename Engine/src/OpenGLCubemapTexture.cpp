#include "../../include/private/Graphics/OpenGL/OpenGLCubemapTexture.hpp"
#include <Logger.hpp>
#include <stb_image.h>
#include <vector>
#include <cmath>

namespace Sleak {
namespace RenderEngine {

OpenGLCubemapTexture::OpenGLCubemapTexture() {}

OpenGLCubemapTexture::~OpenGLCubemapTexture() {
    if (m_texture != 0) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
}

bool OpenGLCubemapTexture::LoadCubemap(const std::array<std::string, 6>& facePaths) {
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_texture);

    stbi_set_flip_vertically_on_load(false);

    for (int i = 0; i < 6; ++i) {
        int w, h, channels;
        unsigned char* pixels = stbi_load(facePaths[i].c_str(), &w, &h, &channels, 3);
        if (!pixels) {
            SLEAK_ERROR("OpenGLCubemapTexture: Failed to load face {}: {}",
                        i, facePaths[i]);
            glDeleteTextures(1, &m_texture);
            m_texture = 0;
            return false;
        }

        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
                     0, GL_RGB,
                     w, h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, pixels);

        if (i == 0) {
            m_width = static_cast<uint32_t>(w);
            m_height = static_cast<uint32_t>(h);
        }

        stbi_image_free(pixels);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_format = TextureFormat::RGB8;
    return true;
}

bool OpenGLCubemapTexture::LoadEquirectangular(const std::string& path,
                                                 uint32_t faceSize) {
    stbi_set_flip_vertically_on_load(false);

    int panW, panH, channels;
    unsigned char* panorama = stbi_load(path.c_str(), &panW, &panH, &channels, 3);
    if (!panorama) {
        SLEAK_ERROR("OpenGLCubemapTexture: Failed to load panorama: {}", path);
        return false;
    }

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_texture);

    m_width = faceSize;
    m_height = faceSize;

    std::vector<unsigned char> faceData(faceSize * faceSize * 3);

    // For each cubemap face, map each pixel to a 3D direction,
    // convert to equirectangular UV, and sample the panorama.
    for (int face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < faceSize; ++y) {
            for (uint32_t x = 0; x < faceSize; ++x) {
                // Map (x, y) to [-1, 1] range
                float u = (2.0f * (x + 0.5f) / faceSize) - 1.0f;
                float v = (2.0f * (y + 0.5f) / faceSize) - 1.0f;

                float dx, dy, dz;
                switch (face) {
                    case 0: dx =  1.0f; dy = -v;    dz = -u;    break; // +X
                    case 1: dx = -1.0f; dy = -v;    dz =  u;    break; // -X
                    case 2: dx =  u;    dy =  1.0f; dz =  v;    break; // +Y
                    case 3: dx =  u;    dy = -1.0f; dz = -v;    break; // -Y
                    case 4: dx =  u;    dy = -v;    dz =  1.0f; break; // +Z
                    case 5: dx = -u;    dy = -v;    dz = -1.0f; break; // -Z
                    default: dx = dy = dz = 0.0f; break;
                }

                // Normalize direction
                float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len; dy /= len; dz /= len;

                // Convert to equirectangular UV
                float lon = std::atan2(dz, dx);       // [-PI, PI]
                float lat = std::asin(std::clamp(dy, -1.0f, 1.0f)); // [-PI/2, PI/2]

                float panU = 0.5f + lon / (2.0f * 3.14159265f);
                float panV = 0.5f - lat / 3.14159265f;

                // Bilinear sample the panorama
                float srcX = panU * (panW - 1);
                float srcY = panV * (panH - 1);
                int x0 = static_cast<int>(srcX);
                int y0 = static_cast<int>(srcY);
                int x1 = std::min(x0 + 1, panW - 1);
                int y1 = std::min(y0 + 1, panH - 1);
                x0 = std::clamp(x0, 0, panW - 1);
                y0 = std::clamp(y0, 0, panH - 1);
                float fx = srcX - x0;
                float fy = srcY - y0;

                size_t idx = (y * faceSize + x) * 3;
                for (int c = 0; c < 3; ++c) {
                    float c00 = panorama[(y0 * panW + x0) * 3 + c];
                    float c10 = panorama[(y0 * panW + x1) * 3 + c];
                    float c01 = panorama[(y1 * panW + x0) * 3 + c];
                    float c11 = panorama[(y1 * panW + x1) * 3 + c];
                    float val = c00 * (1 - fx) * (1 - fy) + c10 * fx * (1 - fy)
                              + c01 * (1 - fx) * fy + c11 * fx * fy;
                    faceData[idx + c] = static_cast<unsigned char>(
                        std::clamp(val, 0.0f, 255.0f));
                }
            }
        }

        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                     0, GL_RGB,
                     faceSize, faceSize, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, faceData.data());
    }

    stbi_image_free(panorama);

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_format = TextureFormat::RGB8;

    SLEAK_INFO("OpenGLCubemapTexture: Loaded equirectangular panorama ({}x{} -> {} face)",
               panW, panH, faceSize);
    return true;
}

bool OpenGLCubemapTexture::LoadGradient(float topR, float topG, float topB,
                                         float midR, float midG, float midB,
                                         float botR, float botG, float botB,
                                         uint32_t resolution) {
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_texture);

    m_width = resolution;
    m_height = resolution;

    std::vector<unsigned char> faceData(resolution * resolution * 3);

    for (int face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < resolution; ++y) {
            // Map y to a direction: top of face = +Y, bottom = -Y
            float v = 1.0f - 2.0f * static_cast<float>(y) / (resolution - 1);

            float r, g, b;
            if (v >= 0.0f) {
                // Lerp from mid to top
                r = midR + (topR - midR) * v;
                g = midG + (topG - midG) * v;
                b = midB + (topB - midB) * v;
            } else {
                // Lerp from mid to bottom
                float t = -v;
                r = midR + (botR - midR) * t;
                g = midG + (botG - midG) * t;
                b = midB + (botB - midB) * t;
            }

            for (uint32_t x = 0; x < resolution; ++x) {
                size_t idx = (y * resolution + x) * 3;
                faceData[idx + 0] = static_cast<unsigned char>(r * 255.0f);
                faceData[idx + 1] = static_cast<unsigned char>(g * 255.0f);
                faceData[idx + 2] = static_cast<unsigned char>(b * 255.0f);
            }
        }

        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                     0, GL_RGB,
                     resolution, resolution, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, faceData.data());
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_format = TextureFormat::RGB8;
    return true;
}

bool OpenGLCubemapTexture::LoadFromMemory(const void* /*data*/, uint32_t /*width*/,
                                           uint32_t /*height*/, TextureFormat /*format*/) {
    SLEAK_ERROR("OpenGLCubemapTexture: LoadFromMemory not supported, use LoadCubemap()");
    return false;
}

bool OpenGLCubemapTexture::LoadFromFile(const std::string& /*filePath*/) {
    SLEAK_ERROR("OpenGLCubemapTexture: LoadFromFile not supported, use LoadCubemap()");
    return false;
}

void OpenGLCubemapTexture::Bind(uint32_t slot) const {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_texture);
}

void OpenGLCubemapTexture::Unbind() const {
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

void OpenGLCubemapTexture::SetFilter(TextureFilter filter) {
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_texture);
    switch (filter) {
        case TextureFilter::Nearest:
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            break;
        case TextureFilter::Linear:
        default:
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

void OpenGLCubemapTexture::SetWrapMode(TextureWrapMode /*wrapMode*/) {
    // Cubemaps always use CLAMP_TO_EDGE
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_texture);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

}  // namespace RenderEngine
}  // namespace Sleak
