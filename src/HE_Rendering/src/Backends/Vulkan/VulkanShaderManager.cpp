#include "Backends/Vulkan/VulkanShaderManager.h"
#include <cstdint>
#include <fstream>
#include <sstream>

ShaderHandle VulkanShaderManager::load(const char* path, HE::ShaderType type)
{
    if (!fs::exists(path))
    {
        Logger::Log(Logger::LogLevel::Error, (std::string("VulkanShaderManager: shader file not found: ") + path).c_str());
        return { 0, false };
    }

    // Vulkan expects SPIR-V bytecode — read as binary
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        Logger::Log(Logger::LogLevel::Error, (std::string("VulkanShaderManager: failed to open shader file: ") + path).c_str());
        return { 0, false };
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    ShaderHandle handle;
    handle.type = type;
    // Generate a unique ID (simple incrementing counter)
    static unsigned int nextId = 1;
    handle.id = nextId++;

    // Create Vulkan shader module
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (m_device != VK_NULL_HANDLE)
    {
        if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            Logger::Log(Logger::LogLevel::Error, "VulkanShaderManager: failed to create shader module");
            return { 0, false };
        }
        m_shaderModules[handle.id] = shaderModule;
    }
    else
    {
        Logger::Log(Logger::LogLevel::Warning, "VulkanShaderManager: device not set, shader module not created");
    }

    m_loadedShaders.push_back(handle);
    return handle;
}

void VulkanShaderManager::release(ShaderHandle handle)
{
    auto it = m_shaderModules.find(handle.id);
    if (it != m_shaderModules.end())
    {
        if (m_device != VK_NULL_HANDLE && it->second != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, it->second, nullptr);
        m_shaderModules.erase(it);
    }
}

bool VulkanShaderManager::compile(ShaderHandle& handle)
{
    // Vulkan uses pre-compiled SPIR-V — "compile" is a no-op if module is created
    auto it = m_shaderModules.find(handle.id);
    if (it != m_shaderModules.end() && it->second != VK_NULL_HANDLE)
    {
        handle.ready = true;
        return true;
    }
    Logger::Log(Logger::LogLevel::Error, "VulkanShaderManager: shader module not found for compile");
    return false;
}

ShaderProgramHandle VulkanShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
    // Vulkan doesn't have a "program" object like OpenGL.
    // Shader stages are bound directly in the pipeline.
    // We'll just store the shader handles for reference.
    ShaderProgramHandle program;
    static unsigned int nextProgramId = 1;
    program.id = nextProgramId++;
    program.shaders = shaders;
    program.ready = true;

    m_loadedPrograms.push_back(program);
    return program;
}

void VulkanShaderManager::releaseProgram(ShaderProgramHandle handle)
{
    // Vulkan: no explicit program object to destroy.
    // Just remove from tracking.
    (void)handle;
}

void VulkanShaderManager::cleanup()
{
    for (const auto& program : m_loadedPrograms)
        releaseProgram(program);
    for (const auto& shader : m_loadedShaders)
        release(shader);

    m_loadedPrograms.clear();
    m_loadedShaders.clear();
    m_shaderModules.clear();
}