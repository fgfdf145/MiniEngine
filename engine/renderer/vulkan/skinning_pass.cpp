#include "skinning_pass.h"

#include "buffer.h"
#include "nvrhi_pass.h"
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

VulkanSkinningPass::VulkanSkinningPass(nvrhi::IDevice* nvrhiDevice, uint32_t frameSlotCount)
    : m_nvrhiDevice(nvrhiDevice)
{
    // Set 0 a mesh's five buffers (bind pose and skin read, vertices, positions and last pose
    // written), set 1 the frame's palette; the tyres' pipeline shares both.
    const auto layout = [&](uint32_t registerSpace, std::initializer_list<nvrhi::BindingLayoutItem> items, const char* failure)
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.registerSpace = registerSpace;
        desc.registerSpaceIsDescriptorSet = true;
        desc.bindingOffsets = ShaderBindingOffsets();
        desc.bindings = items;
        return CreateNvrhiBindingLayout(m_nvrhiDevice, desc, failure);
    };
    using Item = nvrhi::BindingLayoutItem;
    m_meshSetLayout = layout(
        0,
        {Item::RawBuffer_SRV(0),
         Item::RawBuffer_SRV(1),
         Item::RawBuffer_UAV(2),
         Item::RawBuffer_UAV(3),
         Item::RawBuffer_UAV(4),
         Item::PushConstants(0, sizeof(SkinningConstants))},
        "Failed to create the skinning mesh binding layout");
    m_paletteSetLayout = layout(1, {Item::RawBuffer_SRV(0)}, "Failed to create the skinning palette binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "skin.comp.spv", {m_meshSetLayout, m_paletteSetLayout});
    m_tyrePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "tyre_deform.comp.spv", {m_meshSetLayout, m_paletteSetLayout});

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
        m_paletteMapped.push_back(mapped);
        nvrhi::BindingSetDesc desc;
        desc.bindings = {nvrhi::BindingSetItem::RawBuffer_SRV(0, m_paletteHandles.back())};
        m_paletteSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_paletteSetLayout, "Failed to create a joint palette binding set"));
    }
}

VulkanSkinningPass::~VulkanSkinningPass() = default;

nvrhi::BindingSetHandle VulkanSkinningPass::Acquire(const VulkanBuffer& buffer) const
{
    nvrhi::BindingSetDesc desc;
    desc.bindings = {
        nvrhi::BindingSetItem::RawBuffer_SRV(0, buffer.GetBindPoseBuffer()),
        nvrhi::BindingSetItem::RawBuffer_SRV(1, buffer.IsSkinned() ? buffer.GetSkinBuffer() : buffer.GetBindPoseBuffer()),
        nvrhi::BindingSetItem::RawBuffer_UAV(2, buffer.GetVertexBuffer()),
        nvrhi::BindingSetItem::RawBuffer_UAV(3, buffer.GetPositionBuffer()),
        nvrhi::BindingSetItem::RawBuffer_UAV(4, buffer.GetPreviousPositionBuffer()),
        nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinningConstants))};
    return CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_meshSetLayout, "Failed to create a skinning binding set");
}

void VulkanSkinningPass::Record(nvrhi::ICommandList* commandList, uint32_t frameSlot, std::span<const Dispatch> dispatches)
{
    if (dispatches.empty())
    {
        return;
    }

    // The posed buffers rest in their read state (VulkanBuffer: vertex input, shader reads, the ray
    // tracing builds); each one written here is an unordered access target for the dispatches, which
    // also orders the write after last frame's reads.
    const auto setWritten = [&](bool written)
    {
        for (const Dispatch& dispatch : dispatches)
        {
            if (dispatch.set == nullptr)
            {
                continue;
            }
            for (nvrhi::IBuffer* buffer :
                 {dispatch.buffer->GetVertexBuffer(), dispatch.buffer->GetPositionBuffer(), dispatch.buffer->GetPreviousPositionBuffer()})
            {
                commandList->setBufferState(buffer, written ? nvrhi::ResourceStates::UnorderedAccess : buffer->getDesc().initialState);
            }
        }
        commandList->commitBarriers();
    };
    setWritten(true);
    {
        const NvrhiPassScope scope(commandList, {});
        auto* palette = static_cast<glm::mat4*>(m_paletteMapped.at(frameSlot));
        uint32_t used = 0;
        nvrhi::ComputeState state;
        for (const Dispatch& dispatch : dispatches)
        {
            if (dispatch.set == nullptr || dispatch.palette == nullptr ||
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
            state.pipeline = dispatch.tyre ? m_tyrePipeline : m_pipeline;
            state.bindings = {dispatch.set, m_paletteSets.at(frameSlot)};
            commandList->setComputeState(state);
            SkinningConstants constants{};
            constants.vertexCount = dispatch.buffer->GetVertexCount();
            constants.paletteBase = used;
            used += dispatch.jointCount;
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch((constants.vertexCount + kSkinningWorkgroupSize - 1) / kSkinningWorkgroupSize);
        }
    }

    // The posed vertices for everything that reads them this frame: vertex input, the shaders that
    // fetch vertices themselves (ray hit shading) and the ray tracing refits.
    setWritten(false);
}
}
