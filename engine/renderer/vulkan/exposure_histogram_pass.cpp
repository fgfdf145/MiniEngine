#include "exposure_histogram_pass.h"

#include "nvrhi_pass.h"
#include "nvrhi_resources.h"

#include <engine/renderer/exposure.h>

#include <array>
#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
// Must match local_size_x / local_size_y in exposure_histogram.comp.
constexpr uint32_t kWorkgroupSize = 16;
static_assert(
    kWorkgroupSize * kWorkgroupSize == kExposureHistogramBinCount,
    "the shader clears and flushes one bin per invocation");

// The bins, then three colour sums and the pixel count they cover (see exposure_histogram.comp).
constexpr uint32_t kColorWords = 4;
constexpr float kColorScale = 1024.0f;
constexpr VkDeviceSize kHistogramBytes = sizeof(uint32_t) * (kExposureHistogramBinCount + kColorWords);

// Must match HistogramConstants in shaders/vulkan/exposure_histogram.comp.
struct HistogramPushConstants
{
    uint32_t width = 0;
    uint32_t height = 0;
    // 1 when the pixels no geometry covered hold a physical sky and are metered like the rest.
    uint32_t meterBackground = 0;
    // The HDR target is pre-exposed; the shader multiplies by this to meter physical luminance.
    float invPreExposure = 1.0f;
};
}

VulkanExposureHistogramPass::VulkanExposureHistogramPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets)
    : m_nvrhiDevice(nvrhiDevice)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = ShaderBindingOffsets();
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::RawBuffer_UAV(2),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(HistogramPushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, layoutDesc, "Failed to create the exposure histogram binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "exposure_histogram.comp.spv", {m_setLayout});
    CreateHistogramBuffers(targets.GetTransientCopyCount());
    CreateBindingSets(targets);
}

VulkanExposureHistogramPass::~VulkanExposureHistogramPass() = default;

ScenePassId VulkanExposureHistogramPass::Id() const
{
    return ScenePassId::ExposureHistogram;
}

RenderPassIo VulkanExposureHistogramPass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {
        RenderTargetId::SceneTaa,
        RenderTargetId::SceneDepth};

    RenderPassIo io{};
    io.reads = kReads;
    return io;
}

void VulkanExposureHistogramPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // The resolved HDR and depth copies and the histogram buffer are all per frame slot, so one index
    // picks all three. The two images are read where the native passes leave them, in
    // SHADER_READ_ONLY_OPTIMAL (the reads above), so NVRHI need not know them.
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    const HistogramBuffer& histogram = m_histograms.at(slot);
    nvrhi::ICommandList* commandList = frame.commandList;
    {
        const NvrhiPassScope scope(commandList, {});
        // The CPU read this buffer before the frame was submitted, which vkQueueSubmit orders ahead
        // of the clear, so only the clear-to-atomics and atomics-to-host hazards need barriers. Each
        // frame leaves it as the atomics did.
        commandList->beginTrackingBufferState(histogram.handle, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(histogram.handle, nvrhi::ResourceStates::CopyDest);
        commandList->clearBufferUInt(histogram.handle, 0);
        commandList->setBufferState(histogram.handle, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_pipeline;
        state.bindings = {m_bindingSets.at(slot)};
        commandList->setComputeState(state);
        const HistogramPushConstants constants{
            frame.outputExtent.width, frame.outputExtent.height, frame.physicalSky ? 1u : 0u, 1.0f / frame.preExposure};
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(
            (frame.outputExtent.width + kWorkgroupSize - 1) / kWorkgroupSize,
            (frame.outputExtent.height + kWorkgroupSize - 1) / kWorkgroupSize);
    }

    // NVRHI has no state for the host's reads: this barrier stays native.
    VkBufferMemoryBarrier computeToHost{};
    computeToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    computeToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToHost.buffer = histogram.buffer;
    computeToHost.offset = 0;
    computeToHost.size = kHistogramBytes;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &computeToHost,
        0,
        nullptr);
}

void VulkanExposureHistogramPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The sets name the old HDR and depth images. The histogram buffers do not depend on the
    // targets and keep their last results.
    CreateBindingSets(targets);
}

std::optional<glm::vec3> VulkanExposureHistogramPass::GetFrameColor(uint32_t frameSlot) const
{
    const uint32_t* color = m_histograms.at(frameSlot).mapped + kExposureHistogramBinCount;
    if (color[3] == 0u)
    {
        return std::nullopt;
    }
    return glm::vec3(static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2])) /
           (static_cast<float>(color[3]) * kColorScale);
}

std::span<const uint32_t> VulkanExposureHistogramPass::GetHistogram(uint32_t frameSlot) const
{
    return std::span<const uint32_t>(m_histograms.at(frameSlot).mapped, kExposureHistogramBinCount);
}

void VulkanExposureHistogramPass::CreateHistogramBuffers(uint32_t count)
{
    m_histograms.reserve(count);
    for (uint32_t slot = 0; slot < count; ++slot)
    {
        HistogramBuffer& histogram = m_histograms.emplace_back();

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kHistogramBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Coherent, so the CPU sees the GPU's writes once the fence and the host barrier in Record
        // have run, with no invalidate.
        void* mapped = nullptr;
        histogram.handle = CreateNvrhiBuffer(
            m_nvrhiDevice,
            bufferInfo,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            histogram.buffer,
            "Failed to create exposure histogram buffer",
            &mapped);
        // Zeroed so a slot that has never been recorded reads as an empty histogram.
        std::memset(mapped, 0, static_cast<size_t>(kHistogramBytes));
        histogram.mapped = static_cast<const uint32_t*>(mapped);
    }
}

void VulkanExposureHistogramPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    // One set per frame slot.
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SceneTaa, slot)),
            nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
            nvrhi::BindingSetItem::RawBuffer_UAV(2, m_histograms.at(slot).handle, nvrhi::BufferRange(0, kHistogramBytes)),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(HistogramPushConstants))};
        m_bindingSets.push_back(
            CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an exposure histogram binding set"));
    }
}
}
