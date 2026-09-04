#pragma once

#include "common.h"

#include <filesystem>

namespace me
{

// RAII wrapper around a VkShaderModule loaded from a SPIR-V file. Modules are only needed while
// vkCreateGraphicsPipelines runs, so VulkanPipelineSet loads each stage once, builds every
// material variant from it, and lets the module go out of scope afterwards.
class VulkanShaderModule
{
  public:
    VulkanShaderModule(VkDevice device, const std::filesystem::path& path);
    ~VulkanShaderModule();

    VulkanShaderModule(const VulkanShaderModule&) = delete;
    VulkanShaderModule& operator=(const VulkanShaderModule&) = delete;

    VkShaderModule GetHandle() const;

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkShaderModule m_module = VK_NULL_HANDLE;
};
}
