#include "pipeline.h"

#include <fstream>
#include <vector>

namespace me
{

namespace
{
std::vector<char> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open shader file: " + path.string());
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}
}

VulkanShaderModule::VulkanShaderModule(VkDevice device, const std::filesystem::path& path)
    : m_device(device)
{
    const std::vector<char> code = ReadFile(path);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    CheckVulkan(vkCreateShaderModule(m_device, &createInfo, nullptr, &m_module), "Failed to create shader module");
}

VulkanShaderModule::~VulkanShaderModule()
{
    if (m_module != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(m_device, m_module, nullptr);
    }
}

VkShaderModule VulkanShaderModule::GetHandle() const
{
    return m_module;
}
}
