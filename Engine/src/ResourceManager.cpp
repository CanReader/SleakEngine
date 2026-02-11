#include "../../include/private/Graphics/ResourceManager.hpp"
#include <Logger.hpp>

namespace Sleak {
    namespace RenderEngine {

        IMPLEMENT_RESOURCE_MANAGER

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
                    SLEAK_ERROR("Failed to cast object to Shader object: {}",
                                e.what());
                }
            } else {
                SLEAK_ERROR("No shader creation function registered!");
            }
            return nullptr;
        }

    };
}