#include "ddgi_debug_pass.h"

#include "compute_pass_util.h"
#include "ray_scene.h"

#include <array>

namespace me
{

namespace
{
// Must match DdgiDebugConstants in shaders/vulkan/ddgi_debug.comp.
struct DdgiDebugConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    uint32_t view = 0;
    uint32_t frameIndex = 0;
};

bool IsDdgiDebugView(GBufferDebugView view)
{
    return view == GBufferDebugView::RayTraced;
}
}

VulkanDdgiDebugPass::VulkanDdgiDebugPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_device(device),
      m_rayScene(rayScene)
{
    try
    {
        static constexpr std::array<VkDescriptorType, 1> kTypes = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        const std::array<VkDescriptorSetLayout, 3> setLayouts = {frameSetLayout, m_rayScene.GetSetLayout(), m_setLayout};
        CreateComputePipeline(m_device, pipelineCache, setLayouts, "ddgi_debug.comp.spv", sizeof(DdgiDebugConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount(), 1, 1);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanDdgiDebugPass::~VulkanDdgiDebugPass()
{
    DestroyHandles();
}

ScenePassId VulkanDdgiDebugPass::Id() const
{
    return ScenePassId::DdgiDebug;
}

RenderPassIo VulkanDdgiDebugPass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneGi};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanDdgiDebugPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!IsDdgiDebugView(frame.gbufferView) || !m_rayScene.IsReady())
    {
        return;
    }
    DdgiDebugConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.view = static_cast<uint32_t>(frame.gbufferView);
    constants.frameIndex = frame.frameIndex;

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneGi, frame.imageIndex, frame.frameSlot);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    const std::array<VkDescriptorSet, 3> sets = {frame.frameDescriptorSet, m_rayScene.GetSet(frame.frameSlot), m_descriptorSets.at(slot)};
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(
        commandBuffer,
        (frame.extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (frame.extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        1);
}

void VulkanDdgiDebugPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanDdgiDebugPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneGi, slot), VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet write = ImageWrite(m_descriptorSets[slot], 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputInfo);
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

void VulkanDdgiDebugPass::DestroyHandles()
{
    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}
}
