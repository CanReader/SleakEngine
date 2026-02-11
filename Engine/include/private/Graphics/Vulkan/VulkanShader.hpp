#ifndef _VULKANSHADER_H_
#define _VULKANSHADER_H_

#include <vector>
#include "Graphics/Shader.hpp"
#include "vulkan/vulkan_core.h"

namespace Sleak {
    namespace RenderEngine {
    
class ENGINE_API VulkanShader : public Shader {
public:
    VulkanShader(VkDevice dev) : device(dev) {}
    ~VulkanShader();
    
    virtual bool compile(const std::string& shaderPath) override;
    virtual bool compile(const std::string& vert, const std::string& frag) override;
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

    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> ReadFile(const std::string& path);

};

    }
}

#endif