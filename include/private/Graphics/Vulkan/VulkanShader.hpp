#ifndef _VULKANSHADER_H_
#define _VULKANSHADER_H_

#include <vector>
#include "Graphics/Common/Shader.hpp"
#include "vulkan/vulkan_core.h"

namespace Sleak {
    namespace RenderEngine {
    
/// Vulkan vertex+fragment shader pair, loaded from precompiled SPIR-V modules.
class ENGINE_API VulkanShader : public Shader {
public:
    VulkanShader(VkDevice dev) : device(dev) {}
    ~VulkanShader();
    
    /// Loads the combined-path convention's .vert.spv/.frag.spv pair.
    virtual bool compile(const std::string& shaderPath) override;
    /// Loads separate vertex and fragment SPIR-V modules.
    virtual bool compile(const std::string& vert, const std::string& frag) override;
    /// Loads only the vertex stage (used for depth-only/shadow pipelines with no fragment shader).
    bool compileVertexOnly(const std::string& vertPath);
    /// Reads a .spv file and creates a VkShaderModule from it.
    VkShaderModule LoadSPIRV(const char* path);
    /// No-op: Vulkan binds shader stages via pipeline creation, not a runtime bind call.
    virtual void bind() override;

    inline VkShaderModule GetVertexShader() { return vertShader; }
    inline VkShaderModule GetFragmentShader() { return fragShader; }

    inline VkPipelineShaderStageCreateInfo GetVertexInfo() { return vertexInfo;}
    inline VkPipelineShaderStageCreateInfo GetFragInfo() { return fragmentInfo;}

private:
    VkDevice device = nullptr;
    VkShaderModule vertShader = nullptr;
    VkShaderModule fragShader = nullptr;

    VkPipelineShaderStageCreateInfo vertexInfo{};
    VkPipelineShaderStageCreateInfo fragmentInfo{};

    /// Wraps compiled SPIR-V bytecode in a VkShaderModule.
    VkShaderModule createShaderModule(const std::vector<char>& code);
    /// Reads a binary file's full contents (used for .spv loading).
    std::vector<char> ReadFile(const std::string& path);

};

    }
}

#endif