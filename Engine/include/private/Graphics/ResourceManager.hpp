#ifndef _RESOURCEMANAGER_HPP_
#define _RESOURCEMANAGER_HPP_

#include <functional>
#include <mutex>
#include <any>
#include <memory>

#include "BufferBase.hpp"
#include "Shader.hpp"
#include <Runtime/Texture.hpp>

#define IMPLEMENT_RESOURCE_MANAGER \
        std::function<std::any(BufferType, uint32_t, void*)> ResourceManager::BufferCreationFunc; \
        std::function<std::any(const std::string&)> ResourceManager::ShaderCreateFunc;\
        std::function<std::any(const std::string&)> ResourceManager::TextureCreateFunc; \
        std::mutex ResourceManager::threadManager;

namespace Sleak {
    namespace RenderEngine {

        enum class ResourceType : uint8_t {
            Buffer = 0,
            Shader = 1,
            Texture = 2
        };

        class ResourceManager {
        public:
            // Register a function for creating buffers
            template <typename T>
            static void RegisterCreateBuffer(T* instance, BufferBase* (T::*method)(BufferType, uint32_t, void*)) {
                std::lock_guard<std::mutex> lock(threadManager);
                BufferCreationFunc = [instance, method](BufferType type, uint32_t size, void* data) -> std::any {
                    return (instance->*method)(type, size, data);
                };
            }

            // Register a function for creating shaders
            template <typename T>
            static void RegisterCreateShader(T* instance, Shader* (T::*method)(const std::string&)) {
                std::lock_guard<std::mutex> lock(threadManager);
                ShaderCreateFunc = [instance, method](const std::string& path) -> std::any {
                    return (instance->*method)(path);
                };
            }

            // Register a function for creating shaders
            template <typename T>
            static void RegisterCreateTexture(T* instance, Texture* (T::*method)(const std::string&)) {
                std::lock_guard<std::mutex> lock(threadManager);
                TextureCreateFunc = [instance, method](const std::string& path) -> std::any {
                    return (instance->*method)(path);
                };
            }

            static BufferBase* CreateBuffer(BufferType Type, uint32_t Size, void* Data);
            static Shader* CreateShader(const std::string& ShaderPath);
            static Sleak::Texture* CreateTexture(const std::string& TexturePath);
            

        private:
            static std::function<std::any(BufferType, uint32_t, void*)> BufferCreationFunc;
            static std::function<std::any(const std::string&)> ShaderCreateFunc;
            static std::function<std::any(const std::string&)> TextureCreateFunc;

            // Mutex for thread safety
            static std::mutex threadManager;
        };
    };
};

#endif