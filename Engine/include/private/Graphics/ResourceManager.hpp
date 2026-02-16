#ifndef _RESOURCEMANAGER_HPP_
#define _RESOURCEMANAGER_HPP_

#include <functional>
#include <mutex>
#include <any>
#include <memory>
#include <array>

#include "BufferBase.hpp"
#include "Shader.hpp"
#include <Runtime/Texture.hpp>

#define IMPLEMENT_RESOURCE_MANAGER \
        std::function<std::any(BufferType, uint32_t, void*)> ResourceManager::BufferCreationFunc; \
        std::function<std::any(const std::string&)> ResourceManager::ShaderCreateFunc;\
        std::function<std::any(const std::string&)> ResourceManager::TextureCreateFunc; \
        std::function<std::any(const void*, uint32_t, uint32_t, TextureFormat)> ResourceManager::TextureFromMemoryCreateFunc; \
        std::function<std::any(const std::array<std::string, 6>&)> ResourceManager::CubemapTextureCreateFunc; \
        std::function<std::any(const std::string&)> ResourceManager::CubemapPanoramaCreateFunc; \
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

            // Register a function for creating textures
            template <typename T>
            static void RegisterCreateTexture(T* instance, Texture* (T::*method)(const std::string&)) {
                std::lock_guard<std::mutex> lock(threadManager);
                TextureCreateFunc = [instance, method](const std::string& path) -> std::any {
                    return (instance->*method)(path);
                };
            }

            // Register a function for creating textures from memory
            static void RegisterCreateTextureFromMemory(std::function<Texture*(const void*, uint32_t, uint32_t, TextureFormat)> func) {
                std::lock_guard<std::mutex> lock(threadManager);
                TextureFromMemoryCreateFunc = [func](const void* data, uint32_t w, uint32_t h, TextureFormat fmt) -> std::any {
                    return func(data, w, h, fmt);
                };
            }

            // Register a function for creating cubemap textures
            template <typename T>
            static void RegisterCreateCubemapTexture(T* instance, Texture* (T::*method)(const std::array<std::string, 6>&)) {
                std::lock_guard<std::mutex> lock(threadManager);
                CubemapTextureCreateFunc = [instance, method](const std::array<std::string, 6>& paths) -> std::any {
                    return (instance->*method)(paths);
                };
            }

            // Register a function for creating cubemap textures from a single panorama
            template <typename T>
            static void RegisterCreateCubemapTextureFromPanorama(T* instance, Texture* (T::*method)(const std::string&)) {
                std::lock_guard<std::mutex> lock(threadManager);
                CubemapPanoramaCreateFunc = [instance, method](const std::string& path) -> std::any {
                    return (instance->*method)(path);
                };
            }

            static BufferBase* CreateBuffer(BufferType Type, uint32_t Size, void* Data);
            static Shader* CreateShader(const std::string& ShaderPath);
            static Sleak::Texture* CreateTexture(const std::string& TexturePath);
            static Sleak::Texture* CreateTextureFromMemory(const void* data, uint32_t width, uint32_t height, TextureFormat format);
            static Sleak::Texture* CreateCubemapTexture(const std::array<std::string, 6>& FacePaths);
            static Sleak::Texture* CreateCubemapTextureFromPanorama(const std::string& PanoramaPath);


        private:
            static std::function<std::any(BufferType, uint32_t, void*)> BufferCreationFunc;
            static std::function<std::any(const std::string&)> ShaderCreateFunc;
            static std::function<std::any(const std::string&)> TextureCreateFunc;
            static std::function<std::any(const void*, uint32_t, uint32_t, TextureFormat)> TextureFromMemoryCreateFunc;
            static std::function<std::any(const std::array<std::string, 6>&)> CubemapTextureCreateFunc;
            static std::function<std::any(const std::string&)> CubemapPanoramaCreateFunc;

            // Mutex for thread safety
            static std::mutex threadManager;
        };
    };
};

#endif