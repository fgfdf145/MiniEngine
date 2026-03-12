#include "render_pass.h"

#include <log/log.h>

#include <array>

namespace
{
const std::array<VkFormat, 3> kDepthFormats = {
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
    VK_FORMAT_D24_UNORM_S8_UINT
};

bool HasStencilComponent(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}
}

VulkanRenderPass::VulkanRenderPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkFormat swapchainImageFormat,
    VkExtent2D extent,
    const std::vector<VkImageView>& imageViews
)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    m_depthFormat = FindDepthFormat();
    CreateDepthResources(extent);

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = {
        colorAttachment,
        depthAttachment
    };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create render pass");
    m_framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); ++i)
    {
        VkImageView attachments[] = { imageViews[i], m_depthImageView };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]), "Failed to create framebuffer");
    }

    LOG_INFO("Render pass created successfully");
}

VulkanRenderPass::~VulkanRenderPass()
{
    for (VkFramebuffer framebuffer : m_framebuffers)
    {
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    }
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        LOG_INFO("Render pass destroyed");
    }
    if (m_depthImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_depthImageView, nullptr);
    }
    if (m_depthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_depthImage, nullptr);
    }
    if (m_depthImageMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_depthImageMemory, nullptr);
    }
}

VkRenderPass VulkanRenderPass::GetHandle() const
{
    return m_renderPass;
}

const std::vector<VkFramebuffer>& VulkanRenderPass::GetFramebuffers() const
{
    return m_framebuffers;
}

VkFormat VulkanRenderPass::FindDepthFormat() const
{
    for (VkFormat format : kDepthFormats)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
        {
            return format;
        }
    }

    throw std::runtime_error("Failed to find supported depth format");
}

void VulkanRenderPass::CreateDepthResources(VkExtent2D extent)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = extent.width;
    imageInfo.extent.height = extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &m_depthImage), "Failed to create depth image");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(m_device, m_depthImage, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_depthImageMemory), "Failed to allocate depth image memory");
    CheckVulkan(vkBindImageMemory(m_device, m_depthImage, m_depthImageMemory, 0), "Failed to bind depth image memory");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencilComponent(m_depthFormat))
    {
        viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthImageView), "Failed to create depth image view");
}

uint32_t VulkanRenderPass::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        const bool typeMatches = (typeFilter & (1u << i)) != 0;
        const bool propertiesMatch = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && propertiesMatch)
        {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable depth image memory type");
}
