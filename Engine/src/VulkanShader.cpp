#include "../../include/private/Graphics/Vulkan/VulkanShader.hpp"
#include "vulkan/vulkan_core.h"
#include <Logger.hpp>
#include <cstdint>
#include <vector>
#include <fstream>

namespace Sleak {
    namespace RenderEngine {
    
VulkanShader::~VulkanShader() {
    if(device && vertShader)
        vkDestroyShaderModule(device, vertShader, nullptr);
    if(device && fragShader)
        vkDestroyShaderModule(device, fragShader, nullptr);
}

bool VulkanShader::compile(const std::string& path) {
    // Strip .hlsl extension if present, then use .vert.spv/.frag.spv
    std::string basePath = path;
    auto hlslPos = basePath.rfind(".hlsl");
    if (hlslPos != std::string::npos) {
        basePath = basePath.substr(0, hlslPos);
    }
    return compile(basePath + ".vert.spv", basePath + ".frag.spv");
}

bool VulkanShader::compile(const std::string& vert, const std::string& frag) {

    if(!device)
        SLEAK_RETURN_ERR("Shader codes cannot be compiled without a device!")

    auto vertCode = ReadFile(vert);
    auto fragCode = ReadFile(frag);

    if(vertCode.empty() || fragCode.empty())
        SLEAK_RETURN_ERR("Could not read shader files!");

    vertShader = createShaderModule(vertCode);
    if(!vertShader)
        SLEAK_RETURN_ERR("Failed to create vertex shader!");
    
    fragShader = createShaderModule(fragCode);
    if(!fragShader)
        SLEAK_RETURN_ERR("Failed to create fragment shader!");

    vertexInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexInfo.module = vertShader;
    vertexInfo.pSpecializationInfo = nullptr;
    vertexInfo.pName = "main";

    fragmentInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentInfo.module = fragShader;
    fragmentInfo.pSpecializationInfo = nullptr;
    fragmentInfo.pName = "main";

    return true;
}

void VulkanShader::bind() {
    
}

VkShaderModule VulkanShader::createShaderModule(const std::vector<char>& code) {
    VkShaderModule module;
    
    VkShaderModuleCreateInfo ShaderInfo{};
    ShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ShaderInfo.codeSize = code.size();
    ShaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkResult result = vkCreateShaderModule(device, &ShaderInfo, nullptr, &module);
    
    if(result != VK_SUCCESS)
        return nullptr;

    return module;
}

std::vector<char> VulkanShader::ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);

    if (!file.is_open()) 
    {
        SLEAK_ERROR("Can't read {}", path);
        return std::vector<char>(0);
    }

    size_t size = (size_t) file.tellg();
    std::vector<char> buffer(size);
    
    file.seekg(0);
    file.read(buffer.data(), size);
    
    file.close();

    return buffer;
}


    }
}