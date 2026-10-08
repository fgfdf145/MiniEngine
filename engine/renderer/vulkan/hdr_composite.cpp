#include "hdr_composite.h"

#include "compute_pass_util.h"
#include "pipeline.h"
#include "render_pass.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace me
{

namespace
{
// Must match HdrCompositeConstants in shaders/vulkan/hdr_composite.frag.
struct HdrCompositePushConstants
{
    float imageMin[2] = {0.0f, 0.0f};
    float imageMax[2] = {0.0f, 0.0f};
    float uvMin[2] = {0.0f, 0.0f};
    float uvMax[2] = {0.0f, 0.0f};
    float outputScale = 1.0f;
    uint32_t encoding = 0;
};

static_assert(sizeof(HdrCompositePushConstants) == 40, "HdrCompositePushConstants must match the shader's block");

// scRGB's 1.0, in cd/m^2.
constexpr float kScRgbUnitNits = 80.0f;
}

VulkanHdrComposite::VulkanHdrComposite(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkExtent2D extent,
    uint32_t imageCount,
    VkRenderPass compositeRenderPass)
    : m_device(device)
    , m_extent(extent)
{
    try
    {
        CreateLayerImages(physicalDevice, imageCount);
        std::vector<VkImageView> views;
        for (const LayerImage& layer : m_layers)
        {
            views.push_back(layer.view);
        }
        m_uiRenderPass = std::make_unique<VulkanRenderPass>(m_device, kUiFormat, m_extent, views, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CreatePipelines(pipelineCache, compositeRenderPass);
    }
    catch (...)
    {
        Destroy();
        throw;
    }
}

VulkanHdrComposite::~VulkanHdrComposite()
{
    Destroy();
}

VkRenderPass VulkanHdrComposite::GetUiRenderPass() const
{
    return m_uiRenderPass->GetHandle();
}

VkFramebuffer VulkanHdrComposite::GetUiFramebuffer(uint32_t imageIndex) const
{
    return m_uiRenderPass->GetFramebuffers().at(imageIndex);
}

void VulkanHdrComposite::PrepareDrawData(ImGuiFrameSnapshot& snapshot, ImTextureID viewportTexture, ImDrawCallback resetRenderState)
{
    m_viewportQuad = snapshot.ReplaceTextureWithCallback(viewportTexture, &VulkanHdrComposite::CutViewport, this, resetRenderState);
}

void VulkanHdrComposite::BeginUiRecording(VkCommandBuffer commandBuffer, const ImDrawData& drawData)
{
    m_recording = commandBuffer;
    m_displayPos = drawData.DisplayPos;
    m_framebufferScale = drawData.FramebufferScale;
}

void VulkanHdrComposite::CutViewport(const ImDrawList* list, const ImDrawCmd* command)
{
    auto* self = static_cast<VulkanHdrComposite*>(command->UserCallbackData);
    if (self == nullptr || self->m_recording == VK_NULL_HANDLE || list == nullptr)
    {
        return;
    }
    // The image's quad within the command's clip rectangle, in framebuffer pixels, rounded as the
    // rasterizer covers pixel centres.
    const ImGuiCommandQuad quad = CommandQuad(*list, *command);
    const ImVec2 scale = self->m_framebufferScale;
    const ImVec2 offset = self->m_displayPos;
    const float minX = (std::max(quad.min.x, command->ClipRect.x) - offset.x) * scale.x;
    const float minY = (std::max(quad.min.y, command->ClipRect.y) - offset.y) * scale.y;
    const float maxX = (std::min(quad.max.x, command->ClipRect.z) - offset.x) * scale.x;
    const float maxY = (std::min(quad.max.y, command->ClipRect.w) - offset.y) * scale.y;
    const auto clampX = [self](float x)
    {
        return std::clamp(static_cast<int32_t>(std::lround(x)), 0, static_cast<int32_t>(self->m_extent.width));
    };
    const auto clampY = [self](float y)
    {
        return std::clamp(static_cast<int32_t>(std::lround(y)), 0, static_cast<int32_t>(self->m_extent.height));
    };
    const int32_t x0 = clampX(minX);
    const int32_t y0 = clampY(minY);
    const int32_t x1 = clampX(maxX);
    const int32_t y1 = clampY(maxY);
    if (x1 <= x0 || y1 <= y0)
    {
        return;
    }

    VkViewport viewport{};
    viewport.width = static_cast<float>(self->m_extent.width);
    viewport.height = static_cast<float>(self->m_extent.height);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
    vkCmdBindPipeline(self->m_recording, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_cutPipeline);
    vkCmdSetViewport(self->m_recording, 0, 1, &viewport);
    vkCmdSetScissor(self->m_recording, 0, 1, &scissor);
    vkCmdDraw(self->m_recording, 3, 1, 0, 0);
    // The reset command after this one rebinds ImGui's pipeline and state.
}

void VulkanHdrComposite::RecordComposite(
    VkCommandBuffer commandBuffer,
    uint32_t imageIndex,
    VkImageView sceneView,
    bool scRgb,
    float sdrWhiteNits)
{
    m_recording = VK_NULL_HANDLE;
    const VkDescriptorSet set = m_sets.at(imageIndex);
    const VkDescriptorImageInfo uiInfo{m_nearestSampler, m_layers.at(imageIndex).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo sceneInfo{m_linearSampler, sceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // The scene's image changes with the viewport's size, so the set is written each frame; its last
    // use, by this swapchain image's previous command buffer, has finished.
    const std::array<VkWriteDescriptorSet, 2> writes = {
        ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &uiInfo),
        ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sceneInfo)};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    HdrCompositePushConstants constants{};
    if (m_viewportQuad.has_value())
    {
        const ImGuiCommandQuad& quad = *m_viewportQuad;
        constants.imageMin[0] = (quad.min.x - m_displayPos.x) * m_framebufferScale.x;
        constants.imageMin[1] = (quad.min.y - m_displayPos.y) * m_framebufferScale.y;
        constants.imageMax[0] = (quad.max.x - m_displayPos.x) * m_framebufferScale.x;
        constants.imageMax[1] = (quad.max.y - m_displayPos.y) * m_framebufferScale.y;
        constants.uvMin[0] = quad.uvMin.x;
        constants.uvMin[1] = quad.uvMin.y;
        constants.uvMax[0] = quad.uvMax.x;
        constants.uvMax[1] = quad.uvMax.y;
    }
    constants.outputScale = scRgb ? sdrWhiteNits / kScRgbUnitNits : sdrWhiteNits;
    constants.encoding = scRgb ? 0u : 1u;

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_extent.width);
    viewport.height = static_cast<float>(m_extent.height);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0}, m_extent};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositeLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void VulkanHdrComposite::CreateLayerImages(VkPhysicalDevice physicalDevice, uint32_t imageCount)
{
    m_layers.resize(imageCount);
    for (LayerImage& layer : m_layers)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = kUiFormat;
        imageInfo.extent = {m_extent.width, m_extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &layer.image), "Failed to create an HDR UI layer image");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, layer.image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &layer.memory), "Failed to allocate an HDR UI layer image");
        CheckVulkan(vkBindImageMemory(m_device, layer.image, layer.memory, 0), "Failed to bind an HDR UI layer image");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = layer.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kUiFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &layer.view), "Failed to create an HDR UI layer view");
    }
}

void VulkanHdrComposite::CreatePipelines(VkPipelineCache pipelineCache, VkRenderPass compositeRenderPass)
{
    VkPipelineLayoutCreateInfo cutLayoutInfo{};
    cutLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    CheckVulkan(vkCreatePipelineLayout(m_device, &cutLayoutInfo, nullptr, &m_cutLayout), "Failed to create the HDR UI cut layout");
    m_cutPipeline = CreateFullscreenPipeline(m_device, pipelineCache, m_uiRenderPass->GetHandle(), m_cutLayout, "hdr_ui_cut.frag.spv", "HDR UI cut");

    static constexpr std::array<VkDescriptorType, 2> kBindings = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    m_setLayout = CreateComputeSetLayout(m_device, kBindings, VK_SHADER_STAGE_FRAGMENT_BIT);
    const uint32_t setCount = static_cast<uint32_t>(m_layers.size());
    m_descriptorPool = CreateImageDescriptorPool(m_device, setCount, static_cast<uint32_t>(kBindings.size()), 1);
    m_sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, setCount);
    m_nearestSampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
    m_linearSampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);

    VkPushConstantRange pushConstants{};
    pushConstants.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstants.size = sizeof(HdrCompositePushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstants;
    CheckVulkan(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_compositeLayout), "Failed to create the HDR composite layout");
    m_compositePipeline =
        CreateFullscreenPipeline(m_device, pipelineCache, compositeRenderPass, m_compositeLayout, "hdr_composite.frag.spv", "HDR composite");
}

void VulkanHdrComposite::Destroy()
{
    if (m_compositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_compositePipeline, nullptr);
        m_compositePipeline = VK_NULL_HANDLE;
    }
    if (m_compositeLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_compositeLayout, nullptr);
        m_compositeLayout = VK_NULL_HANDLE;
    }
    for (VkSampler* sampler : {&m_nearestSampler, &m_linearSampler})
    {
        if (*sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_sets.clear();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_cutPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_cutPipeline, nullptr);
        m_cutPipeline = VK_NULL_HANDLE;
    }
    if (m_cutLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_cutLayout, nullptr);
        m_cutLayout = VK_NULL_HANDLE;
    }
    m_uiRenderPass.reset();
    for (LayerImage& layer : m_layers)
    {
        if (layer.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, layer.view, nullptr);
        }
        if (layer.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, layer.image, nullptr);
        }
        if (layer.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, layer.memory, nullptr);
        }
    }
    m_layers.clear();
}
}
