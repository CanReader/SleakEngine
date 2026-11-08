#include "../../include/private/Graphics/Common/ResourceManager.hpp"
#include <Core/Logger.hpp>

namespace Sleak {
    namespace RenderEngine {

        IMPLEMENT_RESOURCE_MANAGER
        ResourceManager::TextureFromDescFunc
            ResourceManager::TextureFromDescCreateFunc;

        BufferBase* ResourceManager::CreateBuffer(BufferType Type, uint32_t Size, void* Data) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (BufferCreationFunc) {
                std::any result = BufferCreationFunc(Type, Size, Data);
                try {
                    if (result.has_value()) {
                        BufferBase* buffer = std::any_cast<BufferBase*>(result);
                        return buffer;
                    }
                } catch (const std::exception& e) {
                    SLEAK_ERROR("Failed to cast object to Buffer object: {}", e.what());
                }
            } else {
                SLEAK_ERROR("No buffer creation function registered!");
            }
            return nullptr;
        }

        Shader* ResourceManager::CreateShader(const std::string& ShaderPath) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (ShaderCreateFunc) {
                std::any result = ShaderCreateFunc(ShaderPath);
                try {
                    if (result.has_value()) {
                        Shader* shader = std::any_cast<Shader*>(result);
                        return shader;
                    }
                } catch (const std::exception& e) {
                    SLEAK_ERROR("Failed to cast object to Shader object: {}", e.what());
                }
            } else {
                SLEAK_ERROR("No shader creation function registered!");
            }
            return nullptr;
        }

        Sleak::Texture* ResourceManager::CreateTexture(const std::string& TexturePath) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (TextureCreateFunc) {
                std::any result = TextureCreateFunc(TexturePath);
                try {
                    if (result.has_value()) {
                        Texture* texture = std::any_cast<Texture*>(result);
                        return texture;
                    }
                } catch (const std::exception& e) {
                    SLEAK_ERROR("Failed to cast object to Texture object: {}",
                                e.what());
                }
            } else {
                SLEAK_ERROR("No texture creation function registered!");
            }
            return nullptr;
        }

        Sleak::Texture* ResourceManager::CreateTextureFromMemory(const void* data, uint32_t width, uint32_t height, TextureFormat format, uint32_t maxMipLevels) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (TextureFromMemoryCreateFunc) {
                std::any result = TextureFromMemoryCreateFunc(data, width, height, format, maxMipLevels);
                try {
                    if (result.has_value()) {
                        Texture* texture = std::any_cast<Texture*>(result);
                        return texture;
                    }
                } catch (const std::exception& e) {
                    SLEAK_ERROR("Failed to cast object to Texture (from memory) object: {}",
                                e.what());
                }
            } else {
                SLEAK_ERROR("No texture-from-memory creation function registered!");
            }
            return nullptr;
        }

        /// Creates a texture from a desc and mip levels. Backends without a
        /// desc factory get level 0 through the from-memory one.
        Sleak::Texture* ResourceManager::CreateTextureFromDesc(
            const TextureDesc& desc, const TextureMipLevel* levels,
            uint32_t levelCount) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (!levels || levelCount == 0) {
                SLEAK_ERROR("CreateTextureFromDesc: no mip levels given");
                return nullptr;
            }
            if (TextureFromDescCreateFunc)
                return TextureFromDescCreateFunc(desc, levels, levelCount);
            if (!TextureFromMemoryCreateFunc) {
                SLEAK_ERROR("No texture creation function registered!");
                return nullptr;
            }

            // Fallback: level 0 plus the backend's own mips
            static bool warned = false;
            if (!warned) {
                warned = true;
                SLEAK_WARN(
                    "CreateTexture: this backend has no mip-chain upload; "
                    "using level 0 and backend mips, sRGB ignored");
            }
            std::any result = TextureFromMemoryCreateFunc(
                levels[0].pixels, desc.width, desc.height, desc.format,
                desc.mipLevels);
            try {
                if (result.has_value()) return std::any_cast<Texture*>(result);
            } catch (const std::exception& e) {
                SLEAK_ERROR(
                    "Failed to cast object to Texture (from desc) object: {}",
                    e.what());
            }
            return nullptr;
        }

        Sleak::Texture* ResourceManager::CreateCubemapTexture(const std::array<std::string, 6>& FacePaths) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (CubemapTextureCreateFunc) {
                std::any result = CubemapTextureCreateFunc(FacePaths);
                try {
                    if (result.has_value()) {
                        Texture* texture = std::any_cast<Texture*>(result);
                        return texture;
                    }
                } catch (const std::exception& e) {
                    SLEAK_ERROR("Failed to cast object to CubemapTexture object: {}",
                                e.what());
                }
            } else {
                SLEAK_ERROR("No cubemap texture creation function registered!");
            }
            return nullptr;
        }

        Sleak::Texture* ResourceManager::CreateCubemapTextureFromPanorama(const std::string& PanoramaPath) {
            std::lock_guard<std::mutex> lock(threadManager);
            if (CubemapPanoramaCreateFunc) {
                std::any result = CubemapPanoramaCreateFunc(PanoramaPath);
                try {
                    if (result.has_value()) {
                        Texture* texture = std::any_cast<Texture*>(result);
                        return texture;
                    }
                } catch (const std::exception& e) {
                    SLEAK_ERROR("Failed to cast object to CubemapTexture (panorama) object: {}",
                                e.what());
                }
            } else {
                SLEAK_ERROR("No cubemap panorama creation function registered!");
            }
            return nullptr;
        }

    };
}