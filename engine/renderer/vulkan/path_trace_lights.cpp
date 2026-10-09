#include "path_trace_lights.h"

#include "compute_pass_util.h"
#include "nvrhi_pass.h"
#include "ray_scene.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace me
{

namespace
{
// Must match EmissiveBuildConstants in shaders/vulkan/emissive_lights.comp.
struct EmissiveBuildConstants
{
    uint32_t mode = 0;
    uint32_t count = 0;
    uint32_t level = 0;
    uint32_t depth = 0;
};

// Must match the LIGHT_GRID_* constants in shaders/vulkan/light_grid_common.slang.
constexpr uint32_t kLightGridCells = 64u * 16u * 64u;
constexpr uint32_t kLightGridSlots = 32u;
constexpr uint64_t kLightGridBytes = uint64_t{kLightGridCells} * (1u + kLightGridSlots / 2u) * sizeof(uint32_t);
constexpr uint32_t kWorkgroupSize = 64;
// EmissiveTriangle in emissive_lights_common.slang.
constexpr uint32_t kTriangleBytes = 96;
// The element sizes emissive_lights_common.slang and light_grid_common.slang declare.
constexpr uint32_t kTreeStride = sizeof(glm::vec4);
constexpr uint32_t kEntryStride = 2 * sizeof(uint32_t);

// EmissiveTreeLevelStart in emissive_lights_common.slang: floats before level `level`.
uint32_t TreeLevelStart(uint32_t level)
{
    return 4u + ((1u << (2u * level)) - 4u) / 3u;
}

uint32_t Groups(uint32_t count)
{
    return (count + kWorkgroupSize - 1) / kWorkgroupSize;
}

// The six buffers, at registerSpace: the build writes 0, 1, 3 and 5 (EMISSIVE_LIGHTS_WRITE) and pushes
// its constants; the trace reads all six.
nvrhi::BindingLayoutHandle CreateLayout(nvrhi::IDevice* device, uint32_t registerSpace, bool build)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    const auto written = [build](uint32_t slot)
    {
        return build ? nvrhi::BindingLayoutItem::StructuredBuffer_UAV(slot) : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot);
    };
    desc.bindings = {
        written(0),
        written(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        written(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        written(5)};
    if (build)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(EmissiveBuildConstants)));
    }
    return CreateNvrhiBindingLayout(device, desc, "Failed to create an emissive light binding layout");
}
}

VulkanPathTraceLights::VulkanPathTraceLights(
    nvrhi::IDevice* nvrhiDevice,
    uint32_t frameCount,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_nvrhiDevice(nvrhiDevice),
      m_rayScene(rayScene)
{
    m_buildLayout = CreateLayout(m_nvrhiDevice, 2, true);
    m_traceLayout = CreateLayout(m_nvrhiDevice, 4, false);
    m_pipeline = CreateNvrhiComputePipeline(
        m_nvrhiDevice, "emissive_lights.comp.spv",
        {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_buildLayout, rayScene.GetNvrhiTextureSetLayout()});
    m_gridPipeline = CreateNvrhiComputePipeline(
        m_nvrhiDevice, "light_grid.comp.spv",
        {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_buildLayout, rayScene.GetNvrhiTextureSetLayout()});
    m_slots.resize(frameCount);
    for (Slot& slot : m_slots)
    {
        // Placeholders, so the sets are valid before the first light.
        slot.triangles = CreateBuffer(kTriangleBytes, kTriangleBytes, false, "Emissive triangles");
        slot.tree = CreateBuffer(sizeof(float) * TreeLevelStart(2), kTreeStride, false, "Emissive power tree");
        slot.slotInstances = CreateBuffer(sizeof(uint32_t), sizeof(uint32_t), false, "Emissive slot instances");
        slot.slotBases = CreateBuffer(sizeof(uint32_t), sizeof(uint32_t), true, "Emissive slot bases");
        slot.entries = CreateBuffer(kEntryStride, kEntryStride, true, "Emissive entries");
        slot.grid = CreateBuffer(sizeof(uint32_t), sizeof(uint32_t), false, "Light grid");
        CreateSets(slot);
    }
}

VulkanPathTraceLights::~VulkanPathTraceLights()
{
    for (Slot& slot : m_slots)
    {
        for (Buffer* buffer : {&slot.slotBases, &slot.entries})
        {
            if (buffer->mapped != nullptr)
            {
                m_nvrhiDevice->unmapBuffer(buffer->handle);
            }
        }
    }
}

nvrhi::IBindingLayout* VulkanPathTraceLights::GetTraceLayout() const
{
    return m_traceLayout;
}

nvrhi::IBindingSet* VulkanPathTraceLights::GetTraceSet(uint32_t frameSlot) const
{
    return m_slots.at(frameSlot).traceSet;
}

uint32_t VulkanPathTraceLights::GetLightCount(uint32_t frameSlot) const
{
    return frameSlot < m_slots.size() && m_slots[frameSlot].emissiveBuilt ? m_slots[frameSlot].lightCount : 0u;
}

bool VulkanPathTraceLights::HasLightGrid(uint32_t frameSlot) const
{
    return frameSlot < m_slots.size() && m_slots[frameSlot].gridBuilt;
}

VulkanPathTraceLights::Buffer VulkanPathTraceLights::CreateBuffer(uint64_t size, uint32_t stride, bool hostVisible, const char* name) const
{
    Buffer result{};
    // Whole elements, at least 16 bytes.
    result.size = (std::max<uint64_t>(size, 16) + stride - 1) / stride * stride;
    result.handle = hostVisible ? CreateUploadBuffer(m_nvrhiDevice, result.size, stride, name, &result.mapped)
                                : CreateDeviceBuffer(m_nvrhiDevice, result.size, stride, true, name);
    return result;
}

bool VulkanPathTraceLights::EnsureBuffer(Buffer& buffer, uint64_t size, uint32_t stride, bool hostVisible, const char* name) const
{
    if (buffer.handle && buffer.size >= size)
    {
        return false;
    }
    // A quarter more, so a streamed map's changes do not remake it every time. The old buffer goes
    // once the frames that used it have (NVRHI holds what its command lists reference).
    const uint64_t grown = size + size / 4;
    if (buffer.mapped != nullptr)
    {
        m_nvrhiDevice->unmapBuffer(buffer.handle);
    }
    buffer = CreateBuffer(grown, stride, hostVisible, name);
    return true;
}

void VulkanPathTraceLights::UpdateSlot(Slot& slot)
{
    if (slot.valid && slot.generation == m_rayScene.GetEmissiveGeneration())
    {
        return;
    }
    slot.generation = m_rayScene.GetEmissiveGeneration();
    slot.valid = true;
    const std::vector<RayEmissiveSubmesh>& submeshes = m_rayScene.GetEmissiveSubmeshes();
    const uint32_t slotCapacity = std::max(m_rayScene.GetSlotCapacity(), 1u);
    uint32_t count = 0;
    uint64_t requested = 0;
    for (const RayEmissiveSubmesh& submesh : submeshes)
    {
        count += std::min(submesh.triangleCount, kMaxLights - count);
        requested += submesh.triangleCount;
    }
    bool rewrite = EnsureBuffer(slot.slotBases, sizeof(uint32_t) * slotCapacity, sizeof(uint32_t), true, "Emissive slot bases");
    rewrite |= EnsureBuffer(slot.slotInstances, sizeof(uint32_t) * slotCapacity, sizeof(uint32_t), false, "Emissive slot instances");
    rewrite |= EnsureBuffer(slot.entries, uint64_t{kEntryStride} * std::max(count, 1u), kEntryStride, true, "Emissive entries");
    rewrite |= EnsureBuffer(slot.triangles, uint64_t{kTriangleBytes} * std::max(count, 1u), kTriangleBytes, false, "Emissive triangles");
    uint32_t depth = 1;
    while ((1u << (2u * depth)) < count)
    {
        ++depth;
    }
    rewrite |= EnsureBuffer(slot.tree, sizeof(float) * TreeLevelStart(depth + 1), kTreeStride, false, "Emissive power tree");
    if (rewrite)
    {
        CreateSets(slot);
    }

    auto* bases = static_cast<uint32_t*>(slot.slotBases.mapped);
    std::fill(bases, bases + slot.slotBases.size / sizeof(uint32_t), ~0u);
    auto* entries = static_cast<uint32_t*>(slot.entries.mapped);
    uint32_t written = 0;
    for (const RayEmissiveSubmesh& submesh : submeshes)
    {
        const uint32_t take = std::min(submesh.triangleCount, kMaxLights - written);
        if (take == 0)
        {
            break;
        }
        if (submesh.slot < slotCapacity)
        {
            bases[submesh.slot] = written;
        }
        for (uint32_t triangle = 0; triangle < take; ++triangle)
        {
            entries[2 * (written + triangle)] = submesh.slot;
            entries[2 * (written + triangle) + 1] = triangle;
        }
        written += take;
    }
    if (requested > written)
    {
        LOG_WARN("Emissive lights: {} of {} emissive triangles kept (at most {})", written, requested, kMaxLights);
    }
    slot.lightCount = written;
    slot.depth = depth;
}

void VulkanPathTraceLights::Record(
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameSet,
    nvrhi::IBindingSet* raySet,
    nvrhi::IDescriptorTable* rayTextureTable,
    uint32_t frameSlot,
    uint32_t frameIndex,
    bool emissive,
    bool lightGrid,
    uint32_t localLightCount)
{
    if (frameSlot >= m_slots.size())
    {
        return;
    }
    Slot& slot = m_slots[frameSlot];
    slot.gridBuilt = false;
    slot.emissiveBuilt = false;
    if (emissive)
    {
        UpdateSlot(slot);
    }
    const uint32_t lightCount = emissive ? slot.lightCount : 0u;
    lightGrid = lightGrid && localLightCount > 0;
    if (lightGrid && !slot.gridMade)
    {
        slot.grid = CreateBuffer(kLightGridBytes, sizeof(uint32_t), false, "Light grid");
        slot.gridMade = true;
        CreateSets(slot);
    }
    if (lightCount == 0 && !lightGrid)
    {
        return;
    }
    // The build's dispatches each read what the one before wrote (a UAV barrier between them); the
    // buffers then go back to where the trace reads them. They are this class's own, which NVRHI tracks
    // (keepInitialState), so no scope is needed.
    const std::array<nvrhi::IBuffer*, 4> written = {slot.triangles.handle, slot.tree.handle, slot.slotInstances.handle, slot.grid.handle};
    const auto setWritten = [&](nvrhi::ResourceStates state)
    {
        for (nvrhi::IBuffer* buffer : written)
        {
            commandList->setBufferState(buffer, state);
        }
        commandList->commitBarriers();
    };
    commandList->clearState();
    nvrhi::ComputeState state;
    state.bindings = {frameSet, raySet, slot.buildSet, rayTextureTable};
    if (lightGrid)
    {
        // The grid reads nothing another dispatch here writes.
        setWritten(nvrhi::ResourceStates::UnorderedAccess);
        state.pipeline = m_gridPipeline;
        commandList->setComputeState(state);
        const EmissiveBuildConstants constants{3u, localLightCount, frameIndex, 0u};
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(Groups(kLightGridCells));
        slot.gridBuilt = true;
        if (lightCount == 0)
        {
            setWritten(nvrhi::ResourceStates::ShaderResource);
            commandList->clearState();
            return;
        }
    }
    // Which instance draws each slot is found again every frame; the slots no instance draws stay ~0.
    ClearBufferUInt(commandList, slot.slotInstances.handle, ~0u);
    setWritten(nvrhi::ResourceStates::UnorderedAccess);

    state.pipeline = m_pipeline;
    commandList->setComputeState(state);
    const auto dispatch = [&](uint32_t mode, uint32_t count, uint32_t level, uint32_t threads)
    {
        const EmissiveBuildConstants constants{mode, count, level, slot.depth};
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(Groups(threads));
        setWritten(nvrhi::ResourceStates::UnorderedAccess);
    };
    const uint32_t instances = std::max(m_rayScene.GetInstanceCount(), 1u);
    dispatch(0u, instances, 0u, instances);
    dispatch(1u, slot.lightCount, 0u, 1u << (2u * slot.depth));
    for (uint32_t level = slot.depth; level-- > 0;)
    {
        dispatch(2u, 0u, level, 1u << (2u * level));
    }
    setWritten(nvrhi::ResourceStates::ShaderResource);
    commandList->clearState();
    slot.emissiveBuilt = true;
}

void VulkanPathTraceLights::CreateSets(Slot& slot) const
{
    const auto items = [&](bool build)
    {
        const auto written = [build](uint32_t index, nvrhi::IBuffer* buffer)
        {
            return build ? nvrhi::BindingSetItem::StructuredBuffer_UAV(index, buffer) : nvrhi::BindingSetItem::StructuredBuffer_SRV(index, buffer);
        };
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            written(0, slot.triangles.handle),
            written(1, slot.tree.handle),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, slot.slotBases.handle),
            written(3, slot.slotInstances.handle),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(4, slot.entries.handle),
            written(5, slot.grid.handle)};
        if (build)
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::PushConstants(0, sizeof(EmissiveBuildConstants)));
        }
        return desc;
    };
    slot.buildSet = CreateNvrhiBindingSet(m_nvrhiDevice, items(true), m_buildLayout, "Failed to create an emissive light binding set");
    slot.traceSet = CreateNvrhiBindingSet(m_nvrhiDevice, items(false), m_traceLayout, "Failed to create an emissive light binding set");
}
}
