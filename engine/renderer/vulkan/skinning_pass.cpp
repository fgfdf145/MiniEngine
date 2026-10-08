#include "skinning_pass.h"

#include "buffer.h"
#include "compute_pass_util.h"
#include "nvrhi_resources.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace me
{

namespace
{
// skin.comp's push constants.
struct SkinningConstants
{
    uint32_t vertexCount = 0;
    // Where the submesh's joints start in this frame's palette buffer.
    uint32_t paletteBase = 0;
};

// Must match local_size_x in skin.comp.
constexpr uint32_t kSkinningWorkgroupSize = 64;
}

VulkanSkinningPass::VulkanSkinningPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    uint32_t frameSlotCount)
    : m_device(device)
{
    try
    {
        static constexpr std::array<VkDescriptorType, 5> kMeshTypes = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        static constexpr std::array<VkDescriptorType, 1> kPaletteTypes = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        m_meshSetLayout = CreateComputeSetLayout(m_device, kMeshTypes);
        m_paletteSetLayout = CreateComputeSetLayout(m_device, kPaletteTypes);
        const std::array<VkDescriptorSetLayout, 2> setLayouts = {m_meshSetLayout, m_paletteSetLayout};
        CreateComputePipeline(m_device, pipelineCache, setLayouts, "skin.comp.spv", sizeof(SkinningConstants), m_pipelineLayout, m_pipeline);
        // The tyres' pipeline shares the layout: the same sets and push constants.
        m_tyrePipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "tyre_deform.comp.spv");

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = kMaxSkinnedMeshes * 5 + frameSlotCount;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = kMaxSkinnedMeshes + frameSlotCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool), "Failed to create skinning descriptor pool");

        const VkDeviceSize bytes = sizeof(glm::mat4) * kMaxPaletteMatrices;
        for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer buffer = VK_NULL_HANDLE;
            void* mapped = nullptr;
            m_paletteHandles.push_back(CreateNvrhiBuffer(
                nvrhiDevice,
                bufferInfo,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                buffer,
                "Failed to create joint palette buffer",
                &mapped));
            m_paletteBuffers.push_back(buffer);
            m_paletteMapped.push_back(mapped);

            VkDescriptorSetAllocateInfo setInfo{};
            setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            setInfo.descriptorPool = m_pool;
            setInfo.descriptorSetCount = 1;
            setInfo.pSetLayouts = &m_paletteSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            CheckVulkan(vkAllocateDescriptorSets(m_device, &setInfo, &set), "Failed to allocate joint palette set");
            m_paletteSets.push_back(set);
            const VkDescriptorBufferInfo paletteInfo{buffer, 0, bytes};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &paletteInfo;
            vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanSkinningPass::~VulkanSkinningPass()
{
    DestroyHandles();
}

VkDescriptorSet VulkanSkinningPass::Acquire(const VulkanBuffer& buffer)
{
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_meshSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &setInfo, &set) != VK_SUCCESS)
    {
        LOG_WARN("More than {} skinned submeshes: the rest keep their bind pose", kMaxSkinnedMeshes);
        return VK_NULL_HANDLE;
    }
    const std::array<VkDescriptorBufferInfo, 5> infos = {
        VkDescriptorBufferInfo{buffer.GetBindPoseHandle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{buffer.IsSkinned() ? buffer.GetSkinHandle() : buffer.GetBindPoseHandle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{buffer.GetVertexHandle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{buffer.GetPositionHandle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{buffer.GetPreviousPositionHandle(), 0, VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
    {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].descriptorCount = 1;
        writes[binding].pBufferInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return set;
}

void VulkanSkinningPass::Release(VkDescriptorSet set)
{
    if (set != VK_NULL_HANDLE)
    {
        m_pendingFrees.push_back({set, m_frame});
    }
}

void VulkanSkinningPass::Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, std::span<const Dispatch> dispatches)
{
    // A set released three frames ago can be in no command buffer still executing: the frame slots'
    // fences have come round since.
    ++m_frame;
    std::erase_if(m_pendingFrees, [&](const PendingFree& pending)
                  {
                      if (m_frame - pending.frame < 3)
                      {
                          return false;
                      }
                      vkFreeDescriptorSets(m_device, m_pool, 1, &pending.set);
                      return true;
                  });
    if (dispatches.empty())
    {
        return;
    }

    // Last frame's draws read the buffers this overwrites: an execution dependency on everything
    // before (no memory to make visible for a write after a read).
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 1, 1, &m_paletteSets.at(frameSlot), 0, nullptr);
    VkPipeline bound = VK_NULL_HANDLE;

    auto* palette = static_cast<glm::mat4*>(m_paletteMapped.at(frameSlot));
    uint32_t used = 0;
    for (const Dispatch& dispatch : dispatches)
    {
        if (dispatch.set == VK_NULL_HANDLE || dispatch.palette == nullptr ||
            dispatch.paletteOffset + dispatch.jointCount > dispatch.palette->size())
        {
            continue;
        }
        if (used + dispatch.jointCount > kMaxPaletteMatrices)
        {
            LOG_WARN("More than {} joints skinned this frame: the rest keep their last pose", kMaxPaletteMatrices);
            break;
        }
        std::memcpy(palette + used, dispatch.palette->data() + dispatch.paletteOffset, sizeof(glm::mat4) * dispatch.jointCount);
        const VkPipeline pipeline = dispatch.tyre ? m_tyrePipeline : m_pipeline;
        if (pipeline != bound)
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            bound = pipeline;
        }
        SkinningConstants constants{};
        constants.vertexCount = dispatch.buffer->GetVertexCount();
        constants.paletteBase = used;
        used += dispatch.jointCount;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &dispatch.set, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commandBuffer, (constants.vertexCount + kSkinningWorkgroupSize - 1) / kSkinningWorkgroupSize, 1, 1);
    }

    // The posed vertices for everything that reads them this frame: vertex input, and the shaders
    // that fetch vertices themselves (ray hit shading).
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanSkinningPass::DestroyHandles()
{
    // The buffers and their memory (unmapped as it is freed) go with the handles.
    m_paletteMapped.clear();
    m_paletteBuffers.clear();
    m_paletteHandles.clear();
    m_paletteSets.clear();
    for (VkPipeline* pipeline : {&m_pipeline, &m_tyrePipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* layout : {&m_meshSetLayout, &m_paletteSetLayout})
    {
        if (*layout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
}
}
