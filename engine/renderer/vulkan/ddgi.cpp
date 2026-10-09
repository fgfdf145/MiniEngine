#include "ddgi.h"

#include "compute_pass_util.h"
#include "nvrhi_pass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
// Must match DdgiTraceConstants in shaders/vulkan/ddgi_trace.comp and DdgiUpdateConstants in
// ddgi_update.comp, which share the first 48 bytes and the count.
struct DdgiConstants
{
    glm::vec4 rotation[3]{};
    uint32_t scheduledCount = 0;
    // The trace reads the frame index here, the update the hysteresis as float bits.
    uint32_t frameIndexOrHysteresis = 0;
    // The update's lighting epoch (bits 0 to 7) and geometry epoch (bits 8 to 11).
    uint32_t epochs = 0;
    uint32_t padding = 0;
};
static_assert(sizeof(DdgiConstants) == 64, "DdgiConstants must match ddgi_trace.comp and ddgi_update.comp");
}

VulkanDdgi::VulkanDdgi(
    nvrhi::IDevice* nvrhiDevice,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* raySetLayout,
    uint32_t frameCount,
    bool rayQuery)
    : m_nvrhiDevice(nvrhiDevice),
      m_frameCount(frameCount)
{
    // Irradiance: rgb and the sky visibility. Visibility: the distance's two moments, all it holds,
    // so half the memory of the irradiance's format per texel.
    m_irradiance = CreateAtlas(kDdgiIrradianceTexels, nvrhi::Format::RGBA16_FLOAT, "DDGI irradiance atlas");
    m_visibility = CreateAtlas(kDdgiVisibilityTexels, nvrhi::Format::RG16_FLOAT, "DDGI visibility atlas");
    m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    m_states = CreateDeviceBuffer(
        m_nvrhiDevice, static_cast<uint64_t>(kDdgiProbeStateBytes) * kDdgiProbesPerLevel * kDdgiMaxLevels, kDdgiProbeStateBytes, true, "DDGI probe states");
    m_rays = CreateDeviceBuffer(m_nvrhiDevice, sizeof(glm::vec4) * kDdgiRaysPerProbe * kMaxProbesPerFrame, sizeof(glm::vec4), true, "DDGI rays");
    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        void* mapped = nullptr;
        m_schedules.push_back(CreateUploadBuffer(m_nvrhiDevice, sizeof(uint32_t) * kMaxProbesPerFrame, sizeof(uint32_t), "DDGI schedule", &mapped));
        m_scheduleMapped.push_back(mapped);
        // The update writes its reports on the device; the frame copies them to the slot's readback
        // buffer, which the CPU reads once the slot's fence has signalled.
        m_feedback.push_back(CreateDeviceBuffer(m_nvrhiDevice, sizeof(uint32_t) * kMaxProbesPerFrame, sizeof(uint32_t), true, "DDGI feedback"));
        m_feedbackReadback.push_back(CreateReadbackBuffer(m_nvrhiDevice, sizeof(uint32_t) * kMaxProbesPerFrame, "DDGI feedback readback", &mapped));
        m_feedbackMapped.push_back(mapped);
    }
    m_scheduleCounts.assign(m_frameCount, 0u);
    m_recordedSchedules.resize(m_frameCount);
    m_feedbackPending.assign(m_frameCount, 0u);

    nvrhi::BindingLayoutDesc traceDesc;
    traceDesc.visibility = nvrhi::ShaderType::Compute;
    traceDesc.registerSpace = 2;
    traceDesc.registerSpaceIsDescriptorSet = true;
    traceDesc.bindingOffsets = ShaderBindingOffsets();
    traceDesc.bindings = {
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DdgiConstants))};
    m_traceLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, traceDesc, "Failed to create the DDGI trace binding layout");

    nvrhi::BindingLayoutDesc updateDesc = traceDesc;
    updateDesc.bindings = {
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        nvrhi::BindingLayoutItem::Texture_UAV(2),
        nvrhi::BindingLayoutItem::Texture_UAV(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(5),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DdgiConstants))};
    m_updateLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, updateDesc, "Failed to create the DDGI update binding layout");

    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        nvrhi::BindingSetDesc trace;
        trace.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_schedules[slot]),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_rays),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(DdgiConstants))};
        m_traceSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, trace, m_traceLayout, "Failed to create a DDGI trace binding set"));
        nvrhi::BindingSetDesc update;
        update.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_schedules[slot]),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_rays),
            nvrhi::BindingSetItem::Texture_UAV(2, m_irradiance),
            nvrhi::BindingSetItem::Texture_UAV(3, m_visibility),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_states),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_feedback[slot]),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(DdgiConstants))};
        m_updateSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, update, m_updateLayout, "Failed to create a DDGI update binding set"));
    }

    m_tracePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ddgi_trace.comp.spv", {frameSetLayout, raySetLayout, m_traceLayout});
    if (rayQuery)
    {
        m_rayQueryTracePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ddgi_trace_ray_query.comp.spv", {frameSetLayout, raySetLayout, m_traceLayout});
    }
    m_updatePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ddgi_update.comp.spv", {frameSetLayout, raySetLayout, m_updateLayout});
}

VulkanDdgi::~VulkanDdgi()
{
    for (size_t slot = 0; slot < m_schedules.size(); ++slot)
    {
        m_nvrhiDevice->unmapBuffer(m_schedules[slot]);
    }
    for (size_t slot = 0; slot < m_feedbackReadback.size(); ++slot)
    {
        m_nvrhiDevice->unmapBuffer(m_feedbackReadback[slot]);
    }
}

void VulkanDdgi::SetSchedule(uint32_t frameSlot, std::span<const uint32_t> probes)
{
    const size_t count = std::min<size_t>(probes.size(), kMaxProbesPerFrame);
    if (count > 0)
    {
        std::memcpy(m_scheduleMapped[frameSlot], probes.data(), sizeof(uint32_t) * count);
    }
    m_scheduleCounts[frameSlot] = static_cast<uint32_t>(count);
    m_recordedSchedules[frameSlot].assign(probes.begin(), probes.begin() + static_cast<std::ptrdiff_t>(count));
    m_feedbackPending[frameSlot] = 0u;
}

void VulkanDdgi::TakeFeedback(uint32_t frameSlot, std::vector<uint32_t>& scheduled, std::vector<uint32_t>& feedback)
{
    scheduled.clear();
    feedback.clear();
    if (m_feedbackPending[frameSlot] == 0u)
    {
        return;
    }
    m_feedbackPending[frameSlot] = 0u;
    scheduled = m_recordedSchedules[frameSlot];
    const uint32_t* reports = static_cast<const uint32_t*>(m_feedbackMapped[frameSlot]);
    feedback.assign(reports, reports + scheduled.size());
}

void VulkanDdgi::Invalidate()
{
    m_cleared = false;
}

void VulkanDdgi::Record(
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameSet,
    nvrhi::IBindingSet* raySet,
    uint32_t frameSlot,
    uint32_t frameIndex,
    float hysteresis,
    uint32_t lightingEpoch,
    uint32_t geometryEpoch,
    bool rayQuery)
{
    if (!m_cleared)
    {
        // First use, or new content: every probe black and never updated. The first time the atlases
        // come out of nothing (Common), afterwards from where set 0 samples them.
        const nvrhi::ResourceStates before = m_atlasesInitialized ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
        const NvrhiPassScope scope(
            commandList,
            {{m_irradiance, before, nvrhi::ResourceStates::ShaderResource}, {m_visibility, before, nvrhi::ResourceStates::ShaderResource}},
            {{m_states, nvrhi::ResourceStates::ShaderResource}});
        ClearTextureFloat(commandList, m_irradiance, nvrhi::Color(0.0f));
        ClearTextureFloat(commandList, m_visibility, nvrhi::Color(0.0f));
        ClearBufferUInt(commandList, m_states, 0u);
        m_cleared = true;
        m_atlasesInitialized = true;
    }

    const uint32_t count = m_scheduleCounts[frameSlot];
    if (count == 0)
    {
        return;
    }

    // The atlases and states rest where set 0 reads them; the update writes them.
    const NvrhiPassScope scope(
        commandList,
        {{m_irradiance, nvrhi::ResourceStates::ShaderResource}, {m_visibility, nvrhi::ResourceStates::ShaderResource}},
        {{m_states, nvrhi::ResourceStates::ShaderResource}});

    DdgiConstants constants{};
    const glm::mat3 rotation = DdgiRayRotation(frameIndex);
    for (int column = 0; column < 3; ++column)
    {
        constants.rotation[column] = glm::vec4(rotation[column], 0.0f);
    }
    constants.scheduledCount = count;
    constants.frameIndexOrHysteresis = frameIndex;

    commandList->setBufferState(m_rays, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_schedules[frameSlot], nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    const bool useRayQuery = rayQuery && m_rayQueryTracePipeline;
    nvrhi::ComputeState trace;
    trace.pipeline = useRayQuery ? m_rayQueryTracePipeline : m_tracePipeline;
    trace.bindings = {frameSet, raySet, m_traceSets[frameSlot]};
    commandList->setComputeState(trace);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(count, 1, 1);

    // The trace's rays before the update reads them; the atlases, which the trace sampled through
    // set 0 (its infinite bounce), and the states, written by the update.
    commandList->setBufferState(m_rays, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_irradiance, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(m_visibility, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_states, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_feedback[frameSlot], nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    std::memcpy(&constants.frameIndexOrHysteresis, &hysteresis, sizeof(hysteresis));
    constants.epochs = (lightingEpoch & 0xffu) | ((geometryEpoch & 0xfu) << 8);
    m_feedbackPending[frameSlot] = 1u;
    nvrhi::ComputeState update;
    update.pipeline = m_updatePipeline;
    update.bindings = {frameSet, raySet, m_updateSets[frameSlot]};
    commandList->setComputeState(update);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(count, 1, 1);

    // The reports, to the slot's readback buffer for the CPU; the scope puts the atlases and states
    // back where set 0 reads them.
    commandList->setBufferState(m_feedback[frameSlot], nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(m_feedbackReadback[frameSlot], nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyBuffer(m_feedbackReadback[frameSlot], 0, m_feedback[frameSlot], 0, sizeof(uint32_t) * count);
}

TextureDescriptorBinding VulkanDdgi::GetIrradianceBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_irradiance, m_sampler);
}

TextureDescriptorBinding VulkanDdgi::GetVisibilityBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_visibility, m_sampler);
}

VkBuffer VulkanDdgi::GetProbeStateBuffer() const
{
    return ToNative<VkBuffer>(m_states->getNativeObject(nvrhi::ObjectTypes::VK_Buffer));
}

nvrhi::IBuffer* VulkanDdgi::GetProbeStateHandle() const
{
    return m_states;
}

nvrhi::ITexture* VulkanDdgi::GetIrradianceTexture() const
{
    return m_irradiance;
}

nvrhi::ITexture* VulkanDdgi::GetVisibilityTexture() const
{
    return m_visibility;
}

VkImage VulkanDdgi::GetIrradianceImage() const
{
    return ToNative<VkImage>(m_irradiance->getNativeObject(nvrhi::ObjectTypes::VK_Image));
}

VkImage VulkanDdgi::GetVisibilityImage() const
{
    return ToNative<VkImage>(m_visibility->getNativeObject(nvrhi::ObjectTypes::VK_Image));
}

nvrhi::TextureHandle VulkanDdgi::CreateAtlas(uint32_t texelsPerProbe, nvrhi::Format format, const char* name) const
{
    const uint32_t tile = texelsPerProbe + 2;
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.width = tile * static_cast<uint32_t>(kDdgiGridSize.x * kDdgiGridSize.y);
    desc.height = tile * static_cast<uint32_t>(kDdgiGridSize.z);
    desc.arraySize = kDdgiMaxLevels;
    desc.mipLevels = 1;
    desc.format = format;
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.debugName = name;
    // Where set 0 samples them between frames.
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::TextureHandle texture = m_nvrhiDevice->createTexture(desc);
    if (!texture)
    {
        throw std::runtime_error(std::string("Failed to create the ") + name);
    }
    return texture;
}
}
