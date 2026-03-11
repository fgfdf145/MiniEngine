#pragma once

#include "common.h"

class VulkanPipeline
{
public:
    VulkanPipeline(VkDevice device, VkExtent2D extent, VkRenderPass renderPass, VkDescriptorSetLayout descriptorSetLayout);
    ~VulkanPipeline();

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    VkPipeline GetHandle() const;
    VkPipelineLayout GetLayout() const;

private:
    std::vector<char> ReadFile(const std::string& path) const;
    VkShaderModule CreateShaderModule(const std::vector<char>& code) const;

    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};
