#include "ray_scene.h"

#include "buffer.h"
#include "compute_pass_util.h"
#include "nvrhi_pass.h"
#include "sampler_settings.h"

#include <engine/core/log/log.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
// Must match RayMaterialConstants in shaders/vulkan/ray_material_average.comp.
struct RayMaterialConstants
{
    glm::vec4 baseColorFactor{1.0f};
    // rgb emissive factor, a alpha cutoff.
    glm::vec4 emissiveAndCutoff{0.0f};
    // x output index, y alpha mode, z flags (RAY_MATERIAL_*), w transmission as float bits.
    glm::uvec4 params{0u};
    // RayMaterial::samplers: the slot's textures' indices in the sampler table.
    glm::uvec4 samplers{0u};
};
static_assert(sizeof(RayMaterialConstants) <= 128, "push constants");

// Must match the RAY_MATERIAL_* flags in shaders/vulkan/ray_tracing_common.slang.
constexpr uint32_t kRayMaterialDoubleSided = 1u;
// Alpha tested: hit shading's textured coverage test applies (ray_hit_common.slang).
constexpr uint32_t kRayMaterialAlphaMask = 2u;
// Alpha blended: the textured coverage test takes the alpha as the share of rays it stops.
constexpr uint32_t kRayMaterialAlphaBlend = 4u;
// KHR_materials_transmission: the path tracer's rays meet it and refract through it themselves.
constexpr uint32_t kRayMaterialTransmission = 8u;
// It has a normal map of its own in the ray texture table (RAY_TEXTURE_NORMAL).
constexpr uint32_t kRayMaterialNormalMap = 16u;

// One mesh's buffers as hit shading reads them (RayMeshGeometry in ray_hit_common.slang): the vertex
// and index buffers' device addresses.
struct RayMeshGeometry
{
    VkDeviceAddress vertices = 0;
    VkDeviceAddress indices = 0;
};
static_assert(sizeof(RayMeshGeometry) == 16, "RayMeshGeometry must match ray_hit_common.slang");
// Hit shading reads vertices as floats at these offsets (RAY_VERTEX_* in ray_hit_common.slang).
static_assert(sizeof(Vertex) == 20 * sizeof(float), "RAY_VERTEX_FLOATS in ray_hit_common.slang must match Vertex");
static_assert(offsetof(Vertex, color) == 3 * sizeof(float) && offsetof(Vertex, texCoord) == 6 * sizeof(float) &&
                  offsetof(Vertex, normal) == 8 * sizeof(float) && offsetof(Vertex, tangent) == 11 * sizeof(float) &&
                  offsetof(Vertex, texCoord1) == 15 * sizeof(float),
              "RAY_VERTEX_* offsets in ray_hit_common.slang must match Vertex");

// Matches RayMaterial in ray_tracing_common.slang: albedo and coverage, emission and flags, the
// textures' samplers.
constexpr uint64_t kRayMaterialBytes = 48;
// raySamplers in ray_hit_common.slang: the ray set's binding of every material sampler.
constexpr uint32_t kRaySamplerBinding = 8;
// RAY_TEXTURE_SET in ray_hit_common.slang: the texture table's descriptor set.
constexpr uint32_t kRayTextureSet = 3;

// Every buffer holds at least one element of the largest kind (an instance), so a descriptor always
// names something and the empty scene's dummy instance fits.
uint64_t AtLeastOne(uint64_t size)
{
    return std::max<uint64_t>(size, sizeof(RayInstance));
}

// The element sizes the shaders read the buffers as (ray_tracing_common.slang, ray_hit_common.slang).
constexpr uint32_t kNodeStride = sizeof(BvhNode);
constexpr uint32_t kTriangleStride = sizeof(BvhTriangle);
constexpr uint32_t kInstanceStride = sizeof(RayInstance);
constexpr uint32_t kGeometryStride = sizeof(RayMeshGeometry);
constexpr uint32_t kSourceTriangleStride = sizeof(uint32_t);

// How many instances the frame slots' top-level buffers and acceleration structures hold. A streamed
// world installs a slightly different count every few seconds, and remaking them for each cost ~45 ms
// of allocation in one frame on the GTA map (and waits for the frames in flight), so they keep a
// quarter of room to grow, at least doubling when they do (a map's first load commits over many frames);
// they shrink only once the content needs less than half of them (a small scene after a map).
size_t InstanceCapacity(size_t current, size_t needed)
{
    if (needed <= current && needed * 2 >= current)
    {
        return current;
    }
    return needed > current ? std::max(needed + needed / 4, current * 2) : needed + needed / 4;
}
}

VulkanRayScene::VulkanRayScene(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    nvrhi::vulkan::IDevice* nvrhiVulkanDevice,
    uint32_t frameCount,
    bool hardwareRayTracing,
    TextureDescriptorBinding defaultTexture,
    std::vector<nvrhi::ISampler*> samplerTable,
    bool updateUnusedWhilePending,
    bool nearGpuMemory)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_nvrhiVulkanDevice(nvrhiVulkanDevice),
      m_frameCount(frameCount),
      m_updateUnusedWhilePending(updateUnusedWhilePending),
      m_nearGpuMemory(nearGpuMemory),
      m_defaultTexture(defaultTexture),
      m_samplerTable(std::move(samplerTable))
{
    if (hardwareRayTracing)
    {
        m_acceleration = std::make_unique<VulkanRayAcceleration>(m_physicalDevice, m_device, m_frameCount);
    }
    if (m_samplerTable.size() != VulkanSamplerCache::kSamplerCount)
    {
        throw std::runtime_error("The ray scene needs every material sampler");
    }
    for (uint32_t index = 0; index < m_samplerTable.size(); ++index)
    {
        m_samplerIndices.emplace(m_samplerTable[index], index);
    }
    if (m_defaultTexture.texture == nullptr || m_defaultTexture.nvrhiSampler == nullptr)
    {
        throw std::runtime_error("The ray scene needs a default texture");
    }
    // The lighting pass traces too (its local lights' shadows), from its fragment shader.
    nvrhi::BindingLayoutDesc setDesc;
    setDesc.visibility = nvrhi::ShaderType::Compute | nvrhi::ShaderType::Pixel;
    setDesc.registerSpace = 1;
    setDesc.registerSpaceIsDescriptorSet = true;
    setDesc.bindingOffsets = ShaderBindingOffsets();
    for (uint32_t binding = 0; binding < 5; ++binding)
    {
        setDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(binding));
    }
    if (m_acceleration)
    {
        setDesc.bindings.push_back(nvrhi::BindingLayoutItem::RayTracingAccelStruct(5));
        setDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
        setDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
        setDesc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(kRaySamplerBinding).setSize(VulkanSamplerCache::kSamplerCount));
    }
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, setDesc, "Failed to create the ray scene binding layout");

    // The texture table: as many entries as the device lets one stage see, less a margin for the frame
    // set's own samplers; each content allocates what its slots need. One binding with a variable count
    // (partially bound): a table is allocated for the slots' needs. A new draw's textures go into slots
    // no frame in flight reads, while the frames read others.
    // D3D12: a share of the shader-visible heap (a million descriptors), which the material sets
    // and every other binding set share.
    uint32_t limit = m_physicalDevice != VK_NULL_HANDLE ? 1u << 20 : 1u << 18;
    if (m_physicalDevice != VK_NULL_HANDLE)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
        limit = std::min(properties.limits.maxPerStageDescriptorSampledImages, properties.limits.maxDescriptorSetSampledImages);
    }
    m_textureLimit = std::min<uint32_t>(limit > 256u ? limit - 256u : limit / 2u, 1u << 20);
    nvrhi::BindlessLayoutDesc tableDesc;
    tableDesc.visibility = nvrhi::ShaderType::Compute | nvrhi::ShaderType::Pixel;
    tableDesc.maxCapacity = m_textureLimit;
    // The table's item names binding 0 of its set on Vulkan, and its register space on D3D12 (t0 in
    // space kRayTextureSet, as the shaders declare rayTextures).
    tableDesc.registerSpaces.push_back(
        nvrhi::BindingLayoutItem::Texture_SRV(m_nvrhiDevice->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? 0 : kRayTextureSet));
    tableDesc.descriptorSet = kRayTextureSet;
    tableDesc.variableCount = true;
    tableDesc.updateUnusedWhilePending = m_updateUnusedWhilePending;
    m_textureSetLayout = m_nvrhiDevice->createBindlessLayout(tableDesc);
    if (!m_textureSetLayout)
    {
        throw std::runtime_error("Failed to create the ray texture table layout");
    }

    m_bindingSets.resize(m_frameCount);
    m_sets.assign(m_frameCount, VK_NULL_HANDLE);

    // Placeholder buffers, so the sets are valid before the first build.
    m_meshNodes = CreateBuffer(AtLeastOne(0), kNodeStride);
    m_meshTriangles = CreateBuffer(AtLeastOne(0), kTriangleStride);
    m_meshGeometry = CreateBuffer(AtLeastOne(0), kGeometryStride, true);
    m_sourceTriangles = CreateBuffer(AtLeastOne(0), kSourceTriangleStride, true);
    m_materials = CreateDeviceBuffer(m_nvrhiDevice, kRayMaterialBytes, static_cast<uint32_t>(kRayMaterialBytes), true, "Ray materials");
    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        m_instances.push_back(CreateBuffer(AtLeastOne(0), kInstanceStride, true));
        m_topNodes.push_back(CreateBuffer(AtLeastOne(0), kNodeStride, true));
    }
    WriteSets();

    // The averaging: the materials written, every material sampler (set 0), the slot's textures from the
    // texture table (set 3).
    nvrhi::BindingLayoutDesc averageDesc;
    averageDesc.visibility = nvrhi::ShaderType::Compute;
    averageDesc.registerSpace = 0;
    averageDesc.registerSpaceIsDescriptorSet = true;
    averageDesc.bindingOffsets = ShaderBindingOffsets();
    averageDesc.bindings = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
        nvrhi::BindingLayoutItem::Sampler(1).setSize(VulkanSamplerCache::kSamplerCount),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RayMaterialConstants))};
    m_averageLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, averageDesc, "Failed to create the ray material binding layout");
    m_averagePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ray_material_average.comp.spv", {m_averageLayout, m_textureSetLayout});
    CreateAverageSet();
}

VulkanRayScene::~VulkanRayScene()
{
    // The workers hold their results, which own GPU buffers, and the shared cache.
    if (m_pendingBuild.valid())
    {
        Build pending = m_pendingBuild.get();
        DestroyBuffer(pending.meshNodes);
        DestroyBuffer(pending.meshTriangles);
        DestroyBuffer(pending.meshGeometry);
        DestroyBuffer(pending.sourceTriangles);
    }
    for (TaskFuture<Build>& stale : m_staleBuilds)
    {
        Build build = stale.get();
        DestroyBuffer(build.meshNodes);
        DestroyBuffer(build.meshTriangles);
        DestroyBuffer(build.meshGeometry);
        DestroyBuffer(build.sourceTriangles);
    }
    for (TaskFuture<void>& release : m_releases)
    {
        release.wait();
    }
    DestroyBuffer(m_meshNodes);
    DestroyBuffer(m_meshTriangles);
    DestroyBuffer(m_meshGeometry);
    DestroyBuffer(m_sourceTriangles);
    for (Buffer& buffer : m_spareBuffers)
    {
        DestroyBuffer(buffer);
    }
    for (Buffer& buffer : m_instances)
    {
        DestroyBuffer(buffer);
    }
    for (Buffer& buffer : m_topNodes)
    {
        DestroyBuffer(buffer);
    }
}

void VulkanRayScene::SetContent(
    std::vector<RaySceneSubmesh> submeshes,
    std::vector<glm::mat4> models,
    uint32_t slotCapacity,
    std::span<const RayMaterialSource> placed,
    std::span<const uint32_t> released)
{
    // A build still running is for content that no longer exists. It finishes on its own and puts what
    // it built in the shared cache; nothing waits for it.
    DropFinishedStaleBuilds();
    if (m_pendingBuild.valid())
    {
        m_pendingSuperseded->store(true);
        m_staleBuilds.push_back(std::move(m_pendingBuild));
    }
    auto superseded = std::make_shared<std::atomic<bool>>(false);
    m_pendingSuperseded = superseded;
    // The installed content stays traceable meanwhile (UpdateInstances): streaming changes the content
    // every few seconds, and every effect falling back to its raster version (DDGI to none) until each
    // build installs made the shadows and the indirect light flicker.

    // Slots that lost their submesh hold no material any more.
    for (const uint32_t index : released)
    {
        if (index < m_materialSlots.size())
        {
            m_materialSlots[index] = MaterialSlot{};
        }
    }

    // More slots than the buffer holds: a larger buffer, which every set must name and every
    // material be averaged into again.
    const uint32_t capacity = std::max(slotCapacity, 1u);
    std::vector<uint32_t> rewrite;
    const bool grows = capacity > m_materialCapacity;
    if (grows)
    {
        // The averaged materials carry over and only the slots placed now are averaged: the GPU copies
        // them in the next Record, before any averaging (reading the old buffer from the CPU, video
        // memory the CPU writes well and reads slowly, took over 10 ms on a map).
        nvrhi::BufferHandle grown =
            CreateDeviceBuffer(m_nvrhiDevice, kRayMaterialBytes * capacity, static_cast<uint32_t>(kRayMaterialBytes), true, "Ray materials");
        // Grown twice before a Record: the first growth's copy has not run, so the older buffer is still
        // the one holding the materials. NVRHI keeps a buffer the frames in flight use until they finish.
        if (!m_materialCopy.source && m_materialCapacity > 0)
        {
            m_materialCopy.source = m_materials;
            m_materialCopy.bytes = kRayMaterialBytes * m_materialCapacity;
        }
        m_materials = grown;
        m_materialCapacity = capacity;
        m_materialSlots.resize(capacity);
        CreateAverageSet();
    }
    for (const RayMaterialSource& source : placed)
    {
        if (source.slot >= m_materialCapacity)
        {
            throw std::runtime_error("A ray material slot lies past the slot capacity");
        }
        MaterialSlot& slot = m_materialSlots[source.slot];
        slot.held = true;
        slot.source = source;
        slot.samplers = SamplerIndices(source);
        rewrite.push_back(source.slot);
    }
    // Averaged by the next Record, from the texture table written below.
    m_dirtyMaterialSlots.insert(m_dirtyMaterialSlots.end(), rewrite.begin(), rewrite.end());

    // The texture table: a larger one when the slots outgrew it, with every slot written; otherwise
    // the placed slots get their textures and the released ones the default.
    {
        TextureWrites writes;
        if (capacity > m_textureCapacity)
        {
            if (static_cast<uint64_t>(capacity) * kRayTexturesPerSlot > m_textureLimit)
            {
                throw std::runtime_error("The ray texture table outgrew the device's sampled image limit");
            }
            // The old table goes at once: the frames in flight have finished (SetContent's contract).
            m_textureTable = nullptr;
            m_textureSet = VK_NULL_HANDLE;
            // Room to grow, so a streamed map does not reallocate it at every new cell.
            // A map's draws from the first content on (65,536 slots, 320k descriptors): growing it writes
            // every slot again, which took tens of milliseconds of a frame while a map streamed in.
            m_textureCapacity = std::min<uint32_t>(std::max(capacity + capacity / 4u, 65536u), m_textureLimit / kRayTexturesPerSlot);
            const uint32_t count = m_textureCapacity * kRayTexturesPerSlot;
            m_textureTable = m_nvrhiDevice->createDescriptorTable(m_textureSetLayout);
            if (!m_textureTable)
            {
                throw std::runtime_error("Failed to create the ray texture table");
            }
            m_nvrhiDevice->resizeDescriptorTable(m_textureTable, count, false);
            if (m_textureTable->getCapacity() != count)
            {
                throw std::runtime_error("Failed to allocate the ray texture table");
            }
            if (m_nvrhiVulkanDevice != nullptr)
            {
                m_textureSet = ToNative<VkDescriptorSet>(m_textureTable->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSet));
            }
            writes.infos.reserve(static_cast<size_t>(count));
            for (uint32_t index = 0; index < m_textureCapacity; ++index)
            {
                const bool held = index < m_materialSlots.size() && m_materialSlots[index].held;
                WriteTextureSlot(index, held ? &m_materialSlots[index].source : nullptr, writes);
            }
            // The material averaging reads the table: its set is made again with it.
            CreateAverageSet();
        }
        else
        {
            writes.infos.reserve((placed.size() + released.size()) * kRayTexturesPerSlot);
            // A released slot keeps its descriptors while frames in flight may still read them: no
            // instance names it from this frame on, and the table is partially bound.
            for (const uint32_t index : released)
            {
                if (index < m_textureCapacity && ContentChangeWaitsForFrames())
                {
                    WriteTextureSlot(index, nullptr, writes);
                }
            }
            for (const RayMaterialSource& source : placed)
            {
                WriteTextureSlot(source.slot, &source, writes);
            }
        }
        FlushTextureWrites(writes);
    }

    m_submeshes = std::make_shared<const std::vector<RaySceneSubmesh>>(std::move(submeshes));
    MapInstalledSubmeshes();
    // A new materials buffer, which the frames' sets name (the caller has waited for the frames).
    if (grows)
    {
        WriteSets();
    }

    // What the hardware may treat as opaque: the materials whose coverage the averaging sets to 1. Only
    // the slots that changed hands change, unless every slot was written again.
    const auto opaqueOf = [this](uint32_t index) -> uint8_t
    {
        const MaterialSlot& slot = m_materialSlots[index];
        const GpuMaterialData& material = slot.source.material;
        const bool transmits = (material.shadingModel[0] & kShadingFlagTransmission) != 0u && material.transmissionFactors[0] > 0.0f;
        return slot.held && slot.source.alphaMode == MaterialAlphaMode::Opaque && !transmits ? 1u : 0u;
    };
    if (m_opaqueMaterials.size() != m_materialCapacity)
    {
        m_opaqueMaterials.assign(m_materialCapacity, 0u);
        for (uint32_t index = 0; index < m_materialCapacity; ++index)
        {
            m_opaqueMaterials[index] = opaqueOf(index);
        }
    }
    else
    {
        for (const uint32_t index : released)
        {
            if (index < m_materialCapacity)
            {
                m_opaqueMaterials[index] = opaqueOf(index);
            }
        }
        for (const RayMaterialSource& source : placed)
        {
            m_opaqueMaterials[source.slot] = opaqueOf(source.slot);
        }
    }

    // New materials may emit, or no longer.
    if (!placed.empty() || !released.empty())
    {
        UpdateEmissiveSubmeshes();
    }

    // The hierarchies, on a worker: only meshes not built before cost anything. The previous
    // content's hierarchies go in with it and come back out pruned to what this content uses.
    // The task uses this only to make its buffers, which the destructor waits for.
    m_pendingBuild = RunAsync(
        TaskPriority::Medium,
        [this, submeshes = m_submeshes, models = std::move(models), cache = m_buildCache, superseded]() mutable
        {
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<const MeshData>> meshes;
            std::vector<std::shared_ptr<const VulkanBuffer>> buffers;
            std::vector<uint32_t> slots;
            Build build;
            meshes.reserve(submeshes->size());
            buffers.reserve(submeshes->size());
            slots.reserve(submeshes->size());
            build.flags.reserve(submeshes->size());
            for (const RaySceneSubmesh& submesh : *submeshes)
            {
                meshes.push_back(submesh.mesh);
                buffers.push_back(submesh.buffer);
                build.flags.push_back(submesh.flags);
                slots.push_back(submesh.slot);
            }
            // First the distinct meshes and which need building, then the builds on a few threads
            // (meshes are independent), then the concatenation in submesh order.
            std::unordered_map<const MeshData*, uint32_t> meshIndex;
            std::vector<std::shared_ptr<const MeshData>> distinct;
            std::vector<std::shared_ptr<const MeshBvh>> bvhs;
            for (size_t submesh = 0; submesh < meshes.size(); ++submesh)
            {
                const std::shared_ptr<const MeshData>& mesh = meshes[submesh];
                const MeshData* key = mesh.get();
                const auto [found, inserted] = meshIndex.emplace(key, static_cast<uint32_t>(distinct.size()));
                build.submeshMeshes.push_back(found->second);
                if (!inserted)
                {
                    continue;
                }
                distinct.push_back(mesh);
                build.meshBuffers.push_back(submesh < buffers.size() ? buffers[submesh] : nullptr);
                std::lock_guard lock(cache->mutex);
                const auto cached = cache->meshes.find(key);
                bvhs.push_back(cached != cache->meshes.end() && cached->second.mesh.lock() == mesh ? cached->second.bvh : nullptr);
            }

            std::atomic<size_t> newTriangles{0};
            // Below the frame's own parallel loops, which the task system runs first, and on a quarter
            // of the workers at most: a worker stays in a mesh until it is built (tens of milliseconds
            // for a large one), and with every worker in one the frame's loops ran on its thread alone
            // (50 ms of recording while a map's first load built its hierarchies).
            const uint32_t buildTasks = std::max(1u, TaskSystem::WorkerThreadCount() / 4);
            TaskSystem::ParallelFor(
                static_cast<uint32_t>(distinct.size()),
                std::max(1u, static_cast<uint32_t>((distinct.size() + buildTasks - 1) / buildTasks)),
                [&](uint32_t begin, uint32_t end)
                {
                    for (uint32_t index = begin; index < end; ++index)
                    {
                        // Replaced: the newer build makes what is not in the cache yet (a large change
                        // committing over several frames replaces a build each frame, and each used to
                        // build the same new meshes again alongside the others).
                        if (bvhs[index] || superseded->load())
                        {
                            continue;
                        }
                        const std::shared_ptr<const MeshData>& mesh = distinct[index];
                        const std::vector<glm::vec3> positions =
                            mesh && !mesh->vertices.empty()
                                ? GatherPositions(mesh->vertices.front().position, mesh->vertices.size(), sizeof(Vertex))
                                : std::vector<glm::vec3>{};
                        bvhs[index] = std::make_shared<const MeshBvh>(mesh ? BuildMeshBvh(positions, mesh->indices) : MeshBvh{});
                        newTriangles += bvhs[index]->triangles.size();
                        // At once, for the builds that start while this one runs.
                        std::lock_guard lock(cache->mutex);
                        cache->meshes[mesh.get()] = BuiltMesh{mesh, bvhs[index]};
                    }
                },
                TaskPriority::Medium);

            // A replaced build makes nothing else (concatenating a map's meshes copies hundreds of
            // megabytes, and a large change committing over several frames replaces a build each frame);
            // what it built is in the cache already.
            if (superseded->load())
            {
                return build;
            }
            for (size_t index = 0; index < distinct.size(); ++index)
            {
                AppendMesh(build.scene, *bvhs[index]);
            }
            {
                // Into the cache for the builds after this one; hierarchies of meshes nobody holds any
                // more go, as their address may come back as another mesh's.
                std::lock_guard lock(cache->mutex);
                for (size_t index = 0; index < distinct.size(); ++index)
                {
                    cache->meshes[distinct[index].get()] = BuiltMesh{distinct[index], bvhs[index]};
                }
                std::erase_if(cache->meshes, [](const auto& entry)
                              {
                                  return entry.second.mesh.expired();
                              });
            }
            // The bottom-level acceleration structures of meshes that have none, made here and built on
            // the GPU when this content installs.
            if (m_acceleration)
            {
                // A skinned mesh's is built over its buffer's posed positions and refitted every frame.
                std::vector<VkDeviceAddress> positionAddresses(distinct.size(), 0);
                for (size_t index = 0; index < distinct.size(); ++index)
                {
                    positionAddresses[index] = build.meshBuffers[index] ? build.meshBuffers[index]->GetPositionAddress() : 0;
                }
                build.blas = m_acceleration->Prepare(distinct, bvhs, positionAddresses);
            }
            // Replaced by newer content while it ran: it is never installed, so what it built for the
            // builds after it (the hierarchies, the bottom levels) is all it makes. When the content
            // changes cell after cell, each replaced build used to fill hundreds of megabytes of buffers
            // only to have them freed again, and those frees stalled the frame's thread.
            if (superseded->load())
            {
                return build;
            }
            if (m_acceleration)
            {
                // Hit shading's view of the meshes: each one's buffer addresses, and each leaf
                // triangle's index in its index list, laid out as the triangles are.
                build.meshGeometry = AcquireBuffer(AtLeastOne(sizeof(RayMeshGeometry) * distinct.size()), kGeometryStride, true);
                auto* geometry = static_cast<RayMeshGeometry*>(build.meshGeometry.mapped);
                size_t triangleCount = 0;
                for (size_t index = 0; index < distinct.size(); ++index)
                {
                    const std::shared_ptr<const VulkanBuffer>& buffer = build.meshBuffers[index];
                    geometry[index] = buffer ? RayMeshGeometry{buffer->GetVertexAddress(), buffer->GetIndexAddress()} : RayMeshGeometry{};
                    triangleCount += bvhs[index]->sourceTriangles.size();
                }
                build.sourceTriangles = AcquireBuffer(AtLeastOne(sizeof(uint32_t) * triangleCount), kSourceTriangleStride, true);
                auto* sources = static_cast<uint32_t*>(build.sourceTriangles.mapped);
                for (size_t index = 0; index < distinct.size(); ++index)
                {
                    const std::vector<uint32_t>& meshSources = bvhs[index]->sourceTriangles;
                    if (!meshSources.empty())
                    {
                        std::memcpy(sources, meshSources.data(), sizeof(uint32_t) * meshSources.size());
                    }
                    sources += meshSources.size();
                }
            }
            // The hierarchies into GPU buffers here rather than at install: hundreds of megabytes on a map,
            // in the buffers an earlier content gave back where they are large enough.
            build.meshNodeCount = build.scene.meshNodes.size();
            build.meshTriangleCount = build.scene.meshTriangles.size();
            build.meshNodes = AcquireBuffer(AtLeastOne(sizeof(BvhNode) * build.meshNodeCount), kNodeStride, false);
            build.meshTriangles = AcquireBuffer(AtLeastOne(sizeof(BvhTriangle) * build.meshTriangleCount), kTriangleStride, false);
            if (build.meshNodeCount > 0)
            {
                std::memcpy(build.meshNodes.mapped, build.scene.meshNodes.data(), sizeof(BvhNode) * build.meshNodeCount);
                std::memcpy(build.meshTriangles.mapped, build.scene.meshTriangles.data(), sizeof(BvhTriangle) * build.meshTriangleCount);
            }
            // The CPU keeps the ranges only: the top level reads nothing else of the meshes. The GPU's
            // copies stay host-visible, so CopyCpuScene can read them back.
            build.scene.meshNodes = {};
            build.scene.meshTriangles = {};

            // The top level over where the instances are now; the frames after install update what moved.
            std::vector<RayInstanceInput> inputs;
            inputs.reserve(build.submeshMeshes.size());
            for (size_t index = 0; index < build.submeshMeshes.size() && index < models.size(); ++index)
            {
                inputs.push_back(RayInstanceInput{
                    build.submeshMeshes[index], models[index], slots[index], build.flags[index]});
            }
            if (inputs.size() == build.submeshMeshes.size())
            {
                build.topLevel.Update(build.scene, inputs);
            }

            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            LOG_INFO(
                "Ray scene: {} meshes, {} triangles ({} newly built) in {:.2f} s",
                build.scene.meshes.size(),
                build.meshTriangleCount,
                newTriangles.load(),
                seconds);
            return build;
        });
}

void VulkanRayScene::SetRetire(std::function<void(std::function<void()>)> retire)
{
    m_retire = std::move(retire);
}

bool VulkanRayScene::ContentChangeWaitsForFrames() const
{
    return !m_retire || (m_acceleration && !m_updateUnusedWhilePending);
}

bool VulkanRayScene::HasFinishedBuild() const
{
    return m_pendingBuild.valid() && m_pendingBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void VulkanRayScene::InstallBuild(const std::function<void()>& waitForFrames)
{
    Build build = m_pendingBuild.get();
    // A new capacity replaces every slot's top level and instance buffers, which the frames in flight
    // use; otherwise nothing they use changes.
    // At least the draw slots' capacity, which the renderer reserves ahead for a map that loads over
    // many commits: a new capacity remakes the top levels and waits for the frames in flight.
    const size_t capacity = InstanceCapacity(m_instanceCapacity, std::max<size_t>(build.submeshMeshes.size(), m_materialCapacity));
    const bool framesIdle = capacity != m_instanceCapacity || !m_retire;
    if (framesIdle)
    {
        waitForFrames();
    }
    DropFinishedStaleBuilds();
    m_submeshMeshes = std::move(build.submeshMeshes);
    m_installedFlags = std::move(build.flags);
    // Only the build of the last SetContent installs (the others are stale), so its submeshes are
    // m_submeshes.
    m_installedSubmeshes = m_submeshes;
    m_installedModels.assign(m_installedSubmeshes->size(), glm::mat4(1.0f));
    MapInstalledSubmeshes();
    // The old content goes off the frame's thread, as a superseded build does, once the frames in flight
    // that trace it have finished.
    Build previous;
    previous.scene = std::move(m_scene);
    previous.topLevel = std::move(m_topLevel);
    previous.meshNodes = m_meshNodes;
    previous.meshTriangles = m_meshTriangles;
    previous.meshBuffers = std::move(m_meshBuffers);
    // Without hardware ray tracing the placeholders stay.
    if (build.meshGeometry.handle)
    {
        previous.meshGeometry = m_meshGeometry;
        previous.sourceTriangles = m_sourceTriangles;
    }
    auto retiredContent = std::make_shared<Build>(std::move(previous));
    Retire([this, retiredContent]()
           {
               DiscardBuild(std::move(*retiredContent));
           });
    m_scene = std::move(build.scene);
    m_topLevel = std::move(build.topLevel);
    m_meshNodes = build.meshNodes;
    m_meshTriangles = build.meshTriangles;
    if (build.meshGeometry.handle)
    {
        m_meshGeometry = build.meshGeometry;
        m_sourceTriangles = build.sourceTriangles;
    }
    m_meshBuffers = std::move(build.meshBuffers);
    m_meshNodeCount = build.meshNodeCount;
    m_meshTriangleCount = build.meshTriangleCount;
    if (m_scene.topNodes.empty())
    {
        // Nothing to trace: a root leaf holding one instance every ray skips.
        m_scene.topNodes.push_back(BvhNode{glm::vec3(0.0f), 0u, glm::vec3(0.0f), 1u});
        RayInstance dummy{};
        dummy.data.w = kRayInstanceSkip;
        m_scene.instances.push_back(dummy);
    }

    if (m_acceleration)
    {
        // The bottom levels only the old content held are freed off the frame's thread too.
        auto released = std::make_shared<Build>();
        released->blas = m_acceleration->Install(std::move(build.blas), m_scene.meshes, IncrementalTopLevel::MaxInstances(capacity));
        Retire([this, released]()
               {
                   DiscardBuild(std::move(*released));
               });
    }
    if (capacity != m_instanceCapacity)
    {
        for (uint32_t slot = 0; slot < m_frameCount; ++slot)
        {
            DestroyBuffer(m_instances[slot]);
            DestroyBuffer(m_topNodes[slot]);
            m_instances[slot] = CreateBuffer(AtLeastOne(sizeof(RayInstance) * IncrementalTopLevel::MaxInstances(capacity)), kInstanceStride, true);
            m_topNodes[slot] = CreateBuffer(AtLeastOne(sizeof(BvhNode) * IncrementalTopLevel::MaxNodes(capacity)), kNodeStride, true);
        }
        m_instanceCapacity = capacity;
    }
    // A new top level, which every frame slot copies the worker's into.
    m_slotGenerations.assign(m_frameCount, 0);
    ++m_topLevelGeneration;
    ++m_setsGeneration;
    if (framesIdle)
    {
        WriteSets();
    }
    UpdateEmissiveSubmeshes();
    m_ready = true;
}

void VulkanRayScene::UpdateEmissiveSubmeshes()
{
    m_emissiveSubmeshes.clear();
    ++m_emissiveGeneration;
    m_hasLayeredMaterials = false;
    for (const MaterialSlot& slot : m_materialSlots)
    {
        if (slot.held &&
            ((slot.source.material.shadingModel[0] & (kShadingFlagClearcoat | kShadingFlagSheen | kShadingFlagSpecular)) != 0u ||
             slot.source.normal.texture != nullptr))
        {
            m_hasLayeredMaterials = true;
            break;
        }
    }
    const std::vector<RaySceneSubmesh>& submeshes = *m_installedSubmeshes;
    for (size_t index = 0; index < submeshes.size() && index < m_submeshMeshes.size(); ++index)
    {
        const RaySceneSubmesh& submesh = submeshes[index];
        const uint32_t flags = index < m_installedFlags.size() ? m_installedFlags[index] : 0u;
        if ((flags & (kRayInstanceSkip | kRayInstanceBlend)) != 0u || submesh.slot >= m_materialSlots.size() ||
            !m_materialSlots[submesh.slot].held)
        {
            continue;
        }
        const float* emissive = m_materialSlots[submesh.slot].source.material.emissiveFactor;
        if (std::max({emissive[0], emissive[1], emissive[2]}) <= 0.0f)
        {
            continue;
        }
        // The mesh's leaf triangles run up to the next mesh's (AppendMesh concatenates them in order).
        const uint32_t mesh = m_submeshMeshes[index];
        if (mesh >= m_scene.meshes.size() || m_scene.meshes[mesh].nodeCount == 0)
        {
            continue;
        }
        const uint32_t end = mesh + 1 < m_scene.meshes.size() ? m_scene.meshes[mesh + 1].triangleOffset : static_cast<uint32_t>(m_meshTriangleCount);
        const uint32_t count = end - m_scene.meshes[mesh].triangleOffset;
        if (count > 0)
        {
            m_emissiveSubmeshes.push_back(RayEmissiveSubmesh{submesh.slot, count});
        }
    }
}

void VulkanRayScene::MapInstalledSubmeshes()
{
    // A draw slot holds one submesh at a time; the same slot with the same mesh is the same draw.
    std::vector<uint32_t> currentBySlot(m_materialCapacity, kNoSubmesh);
    const std::vector<RaySceneSubmesh>& submeshes = *m_submeshes;
    const std::vector<RaySceneSubmesh>& installedSubmeshes = *m_installedSubmeshes;
    for (uint32_t index = 0; index < static_cast<uint32_t>(submeshes.size()); ++index)
    {
        if (submeshes[index].slot < currentBySlot.size())
        {
            currentBySlot[submeshes[index].slot] = index;
        }
    }
    m_installedToCurrent.assign(installedSubmeshes.size(), kNoSubmesh);
    for (size_t index = 0; index < installedSubmeshes.size(); ++index)
    {
        const RaySceneSubmesh& installed = installedSubmeshes[index];
        if (installed.slot < currentBySlot.size())
        {
            const uint32_t current = currentBySlot[installed.slot];
            if (current != kNoSubmesh && submeshes[current].mesh == installed.mesh)
            {
                m_installedToCurrent[index] = current;
            }
        }
    }
}

void VulkanRayScene::UpdateInstances(uint32_t frameSlot, std::span<const glm::mat4> models, std::span<const uint8_t> movingInstances)
{
    // The slot's last frame has finished: its set may name the installed content now.
    if (frameSlot < m_slotSetsGenerations.size() && m_slotSetsGenerations[frameSlot] != m_setsGeneration)
    {
        WriteSet(frameSlot);
    }
    if (!m_ready || models.size() != m_submeshes->size() || m_installedToCurrent.size() != m_submeshMeshes.size())
    {
        return;
    }
    std::vector<RayInstanceInput> inputs;
    inputs.reserve(m_submeshMeshes.size());
    for (uint32_t index = 0; index < static_cast<uint32_t>(m_submeshMeshes.size()); ++index)
    {
        const uint32_t current = m_installedToCurrent[index];
        // Each submesh's own flags (RaySceneSubmesh::flags: Blend surfaces only the path tracer's rays
        // meet, far levels of detail none, what casts no shadow): submeshes streamed out since this
        // content installed are skipped by every ray.
        const uint32_t ownFlags = index < m_installedFlags.size() ? m_installedFlags[index] : 0u;
        const bool moving = current != kNoSubmesh && current < movingInstances.size() && movingInstances[current] != 0;
        const uint32_t flags = (ownFlags & kRayInstanceSkip) != 0u || current == kNoSubmesh ? kRayInstanceSkip
                                                                                             : ownFlags | (moving ? kRayInstanceDynamic : 0u);
        if (current != kNoSubmesh)
        {
            m_installedModels[index] = models[current];
        }
        inputs.push_back(RayInstanceInput{m_submeshMeshes[index], m_installedModels[index], (*m_installedSubmeshes)[index].slot, flags});
    }
    // Rebuilt only where something moved (IncrementalTopLevel), and copied to a frame slot only when the
    // slot holds an older one.
    if (m_topLevel.Update(m_scene, inputs))
    {
        if (m_scene.topNodes.empty())
        {
            // Nothing to trace: a root leaf holding one instance every ray skips.
            m_scene.topNodes.push_back(BvhNode{glm::vec3(0.0f), 0u, glm::vec3(0.0f), 1u});
            RayInstance dummy{};
            dummy.data.w = kRayInstanceSkip;
            m_scene.instances.push_back(dummy);
        }
        ++m_topLevelGeneration;
    }
    if (m_acceleration)
    {
        m_acceleration->UpdateTopLevel(frameSlot, m_scene, m_topLevelGeneration, m_opaqueMaterials);
    }
    if (frameSlot < m_slotGenerations.size() && m_slotGenerations[frameSlot] == m_topLevelGeneration)
    {
        return;
    }
    std::memcpy(m_topNodes[frameSlot].mapped, m_scene.topNodes.data(), sizeof(BvhNode) * m_scene.topNodes.size());
    std::memcpy(m_instances[frameSlot].mapped, m_scene.instances.data(), sizeof(RayInstance) * m_scene.instances.size());
    if (frameSlot < m_slotGenerations.size())
    {
        m_slotGenerations[frameSlot] = m_topLevelGeneration;
    }
}

void VulkanRayScene::Record(nvrhi::ICommandList* commandList, uint32_t frameSlot, bool hardwareRays)
{
    // The materials buffer rests as a shader resource (keepInitialState), which the native passes read
    // through the ray set; the copy and the averaging move it and put it back.
    commandList->clearState();
    if (m_materialCopy.source)
    {
        commandList->setBufferState(m_materialCopy.source, nvrhi::ResourceStates::CopySource);
        commandList->setBufferState(m_materials, nvrhi::ResourceStates::CopyDest);
        commandList->commitBarriers();
        commandList->copyBuffer(m_materials, 0, m_materialCopy.source, 0, m_materialCopy.bytes);
        m_materialCopy = MaterialCopy{};
    }
    if (!m_dirtyMaterialSlots.empty() && m_averagePipeline && m_textureTable)
    {
        commandList->setBufferState(m_materials, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_averagePipeline;
        state.bindings = {m_averageSet, m_textureTable};
        commandList->setComputeState(state);
        for (const uint32_t index : m_dirtyMaterialSlots)
        {
            // A slot that lost its submesh again before this record has nothing to average.
            if (index >= m_materialSlots.size() || !m_materialSlots[index].held)
            {
                continue;
            }
            const RayMaterialSource& submesh = m_materialSlots[index].source;
            RayMaterialConstants constants{};
            constants.baseColorFactor = glm::make_vec4(submesh.material.baseColorFactor);
            constants.emissiveAndCutoff = glm::vec4(glm::make_vec3(submesh.material.emissiveFactor), submesh.material.alphaCutoff);
            // Transmission lets light through the surface; the probe rays see it as uncovered.
            float transmission = 0.0f;
            if ((submesh.material.shadingModel[0] & kShadingFlagTransmission) != 0u)
            {
                transmission = std::clamp(submesh.material.transmissionFactors[0], 0.0f, 1.0f);
            }
            uint32_t transmissionBits = 0;
            std::memcpy(&transmissionBits, &transmission, sizeof(transmissionBits));
            constants.params = glm::uvec4(
                index,
                static_cast<uint32_t>(submesh.alphaMode),
                (submesh.doubleSided ? kRayMaterialDoubleSided : 0u) | (submesh.alphaMode == MaterialAlphaMode::Mask ? kRayMaterialAlphaMask : 0u) |
                    (submesh.alphaMode == MaterialAlphaMode::Blend ? kRayMaterialAlphaBlend : 0u) | (transmission > 0.0f ? kRayMaterialTransmission : 0u) |
                    (submesh.normal.texture != nullptr ? kRayMaterialNormalMap : 0u),
                transmissionBits);
            constants.samplers = glm::uvec4(m_materialSlots[index].samplers, 0u, 0u);
            // Each dispatch writes its own slot: no barrier between them.
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch(1, 1, 1);
        }
        m_dirtyMaterialSlots.clear();
    }
    // The materials just written, before any trace.
    commandList->setBufferState(m_materials, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    commandList->clearState();

    // The acceleration structures read only what the worker and UpdateInstances wrote from the host,
    // which a submission makes visible by itself.
    if (m_acceleration)
    {
        m_acceleration->Record(ToNative<VkCommandBuffer>(commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer)), frameSlot, hardwareRays && m_ready);
    }
}

void VulkanRayScene::DiscardBuild(Build build)
{
    std::erase_if(m_releases, [](const TaskFuture<void>& release)
                  {
                      return release.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                  });
    m_releases.push_back(RunAsync(
        TaskPriority::Low,
        [this, build = std::move(build)]() mutable
        {
            RecycleBuffer(build.meshNodes);
            RecycleBuffer(build.meshTriangles);
            RecycleBuffer(build.meshGeometry);
            RecycleBuffer(build.sourceTriangles);
            build = Build{};
        }));
}

void VulkanRayScene::DropFinishedStaleBuilds()
{
    for (auto stale = m_staleBuilds.begin(); stale != m_staleBuilds.end();)
    {
        if (stale->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            DiscardBuild(stale->get());
            stale = m_staleBuilds.erase(stale);
            continue;
        }
        ++stale;
    }
}

bool VulkanRayScene::IsReady() const
{
    return m_ready;
}

bool VulkanRayScene::TakeAccelerationCompleted()
{
    return m_acceleration && m_acceleration->TakeBuildsCompleted();
}

bool VulkanRayScene::HasHardwareRayTracing() const
{
    return m_acceleration != nullptr;
}

bool VulkanRayScene::IsBuilding() const
{
    return m_pendingBuild.valid();
}

VkDescriptorSetLayout VulkanRayScene::GetSetLayout() const
{
    return ToNative<VkDescriptorSetLayout>(m_setLayout->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSetLayout));
}

VkDescriptorSet VulkanRayScene::GetSet(uint32_t frameSlot) const
{
    return m_sets[frameSlot];
}

VkDescriptorSetLayout VulkanRayScene::GetTextureSetLayout() const
{
    return m_textureSetLayout && m_nvrhiVulkanDevice != nullptr
               ? ToNative<VkDescriptorSetLayout>(m_textureSetLayout->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSetLayout))
               : VK_NULL_HANDLE;
}

VkDescriptorSet VulkanRayScene::GetTextureSet() const
{
    return m_textureSet;
}

nvrhi::IBindingLayout* VulkanRayScene::GetNvrhiSetLayout() const
{
    return m_setLayout;
}

nvrhi::IBindingSet* VulkanRayScene::GetBindingSet(uint32_t frameSlot) const
{
    return m_bindingSets[frameSlot];
}

nvrhi::IBindingLayout* VulkanRayScene::GetNvrhiTextureSetLayout() const
{
    return m_textureSetLayout;
}

nvrhi::IDescriptorTable* VulkanRayScene::GetTextureTable() const
{
    return m_textureTable;
}

void VulkanRayScene::WriteTextureSlot(uint32_t slot, const RayMaterialSource* source, TextureWrites& writes) const
{
    // The order of kRayTexturesPerSlot (RAY_TEXTURE_* in ray_hit_common.slang); the samplers are the
    // slot's RayMaterial's (SamplerIndices).
    using Bindings = std::array<TextureDescriptorBinding, kRayTexturesPerSlot>;
    const Bindings bindings = source != nullptr
                                  ? Bindings{source->baseColor, source->metallic, source->roughness, source->emissive, source->normal}
                                  : Bindings{m_defaultTexture, m_defaultTexture, m_defaultTexture, m_defaultTexture, m_defaultTexture};
    if (m_textureSet == VK_NULL_HANDLE)
    {
        for (uint32_t index = 0; index < kRayTexturesPerSlot; ++index)
        {
            const TextureDescriptorBinding& binding = bindings[index].texture != nullptr ? bindings[index] : m_defaultTexture;
            writes.entries.emplace_back(slot * kRayTexturesPerSlot + index, binding.texture);
        }
        return;
    }
    // infos was reserved for every write, so the pointers taken below stay valid.
    const size_t first = writes.infos.size();
    for (uint32_t index = 0; index < kRayTexturesPerSlot; ++index)
    {
        const TextureDescriptorBinding& binding = bindings[index].texture != nullptr ? bindings[index] : m_defaultTexture;
        const VkImageView view = binding.imageView != VK_NULL_HANDLE
                                     ? binding.imageView
                                     : ToNative<VkImageView>(binding.texture->getNativeView(nvrhi::ObjectTypes::VK_ImageView));
        writes.infos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_textureSet;
    write.dstBinding = 0;
    write.dstArrayElement = slot * kRayTexturesPerSlot;
    write.descriptorCount = kRayTexturesPerSlot;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &writes.infos[first];
    writes.writes.push_back(write);
}

void VulkanRayScene::FlushTextureWrites(TextureWrites& writes) const
{
    if (!writes.writes.empty())
    {
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.writes.size()), writes.writes.data(), 0, nullptr);
    }
    for (const auto& [index, texture] : writes.entries)
    {
        m_nvrhiDevice->writeDescriptorTable(m_textureTable, nvrhi::BindingSetItem::Texture_SRV(index, texture));
    }
    writes = TextureWrites{};
}

glm::uvec2 VulkanRayScene::SamplerIndices(const RayMaterialSource& source) const
{
    // WriteTextureSlot's textures: the default one where the source has none.
    const auto indexOf = [this](const TextureDescriptorBinding& binding)
    {
        nvrhi::ISampler* sampler = binding.texture != nullptr ? binding.nvrhiSampler : m_defaultTexture.nvrhiSampler;
        const auto found = m_samplerIndices.find(sampler);
        if (found == m_samplerIndices.end())
        {
            throw std::runtime_error("A ray texture's sampler is not in the sampler table");
        }
        return found->second;
    };
    return glm::uvec2(
        indexOf(source.baseColor) | (indexOf(source.metallic) << 8u) | (indexOf(source.roughness) << 16u) | (indexOf(source.emissive) << 24u),
        indexOf(source.normal));
}

size_t VulkanRayScene::GetSubmeshCount() const
{
    return m_submeshMeshes.size();
}

const std::vector<RayEmissiveSubmesh>& VulkanRayScene::GetEmissiveSubmeshes() const
{
    return m_emissiveSubmeshes;
}

uint64_t VulkanRayScene::GetEmissiveGeneration() const
{
    return m_emissiveGeneration;
}

uint32_t VulkanRayScene::GetSlotCapacity() const
{
    return m_materialCapacity;
}

uint32_t VulkanRayScene::GetInstanceCount() const
{
    return static_cast<uint32_t>(m_scene.instances.size());
}

bool VulkanRayScene::HasLayeredMaterials() const
{
    return m_hasLayeredMaterials;
}

RayScene VulkanRayScene::CopyCpuScene() const
{
    RayScene scene = m_scene;
    if (m_ready && m_meshNodeCount > 0)
    {
        const auto* nodes = static_cast<const BvhNode*>(m_meshNodes.mapped);
        const auto* triangles = static_cast<const BvhTriangle*>(m_meshTriangles.mapped);
        scene.meshNodes.assign(nodes, nodes + m_meshNodeCount);
        scene.meshTriangles.assign(triangles, triangles + m_meshTriangleCount);
    }
    return scene;
}

std::vector<ReferenceMaterial> VulkanRayScene::ReadMaterials() const
{
    std::vector<ReferenceMaterial> materials;
    if (!m_ready || !m_materials)
    {
        return materials;
    }
    // Device local: copied to a readback buffer and waited for (the caller has the GPU idle anyway).
    const uint64_t bytes = kRayMaterialBytes * m_materialCapacity;
    void* mapped = nullptr;
    nvrhi::BufferHandle readback = CreateReadbackBuffer(m_nvrhiDevice, bytes, "Ray materials readback", &mapped);
    nvrhi::CommandListHandle commandList = m_nvrhiDevice->createCommandList();
    commandList->open();
    commandList->copyBuffer(readback, 0, m_materials, 0, bytes);
    commandList->close();
    m_nvrhiDevice->executeCommandList(commandList);
    m_nvrhiDevice->waitForIdle();
    const auto* values = static_cast<const glm::vec4*>(mapped);
    constexpr size_t kStride = kRayMaterialBytes / sizeof(glm::vec4);
    materials.resize(m_materialCapacity);
    for (size_t index = 0; index < materials.size(); ++index)
    {
        const glm::vec4 albedoCoverage = values[index * kStride];
        const glm::vec4 emissionFlags = values[index * kStride + 1];
        uint32_t flags = 0;
        std::memcpy(&flags, &emissionFlags.w, sizeof(flags));
        materials[index].albedo = glm::vec3(albedoCoverage);
        materials[index].coverage = albedoCoverage.w;
        materials[index].emission = glm::vec3(emissionFlags);
        materials[index].doubleSided = (flags & kRayMaterialDoubleSided) != 0u;
    }
    m_nvrhiDevice->unmapBuffer(readback);
    return materials;
}

VulkanRayScene::Buffer VulkanRayScene::CreateBuffer(uint64_t size, uint32_t stride, bool nearGpu) const
{
    Buffer result{};
    // Whole elements: a structured view holds size / stride of them.
    result.size = (size + stride - 1) / stride * stride;
    result.stride = stride;
    result.nearGpu = nearGpu;
    nvrhi::BufferDesc desc;
    desc.byteSize = result.size;
    desc.structStride = stride;
    desc.canHaveRawViews = true;
    desc.cpuAccess = nvrhi::CpuAccessMode::Write;
    desc.debugName = "Ray scene buffer";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    // Host visible: the CPU writes the hierarchies and every frame's instances straight in. On a
    // discrete GPU with resizable BAR the buffers every hit reads go to video memory, which the CPU writes
    // as well; where there is none, or no room, they stay in system memory.
    if (nearGpu && m_nearGpuMemory)
    {
        desc.cpuWriteNearGpu = true;
        result.handle = m_nvrhiDevice->createBuffer(desc);
    }
    if (!result.handle)
    {
        desc.cpuWriteNearGpu = false;
        result.handle = m_nvrhiDevice->createBuffer(desc);
    }
    if (!result.handle)
    {
        throw VulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, "Failed to create a ray scene buffer");
    }
    result.mapped = m_nvrhiDevice->mapBuffer(result.handle, nvrhi::CpuAccessMode::Write);
    if (result.mapped == nullptr)
    {
        throw std::runtime_error("Failed to map a ray scene buffer");
    }
    return result;
}

void VulkanRayScene::DestroyBuffer(Buffer& buffer) const
{
    if (buffer.handle && buffer.mapped != nullptr)
    {
        m_nvrhiDevice->unmapBuffer(buffer.handle);
    }
    buffer = Buffer{};
}

VulkanRayScene::Buffer VulkanRayScene::AcquireBuffer(uint64_t size, uint32_t stride, bool nearGpu)
{
    {
        const std::lock_guard lock(m_spareMutex);
        // The smallest spare of the kind that holds it, unless it would waste more than it holds.
        auto best = m_spareBuffers.end();
        for (auto spare = m_spareBuffers.begin(); spare != m_spareBuffers.end(); ++spare)
        {
            if (spare->nearGpu == nearGpu && spare->stride == stride && spare->size >= size && spare->size <= 2 * size &&
                (best == m_spareBuffers.end() || spare->size < best->size))
            {
                best = spare;
            }
        }
        if (best != m_spareBuffers.end())
        {
            const Buffer buffer = *best;
            m_spareBuffers.erase(best);
            return buffer;
        }
    }
    // Room to grow, so the next content, a little larger, fits in it again.
    return size >= kRecycledBufferBytes ? CreateBuffer(size + size / 4, stride, nearGpu) : CreateBuffer(size, stride, nearGpu);
}

void VulkanRayScene::RecycleBuffer(Buffer& buffer)
{
    if (!buffer.handle || buffer.size < kRecycledBufferBytes)
    {
        DestroyBuffer(buffer);
        return;
    }
    Buffer evicted;
    {
        const std::lock_guard lock(m_spareMutex);
        m_spareBuffers.push_back(buffer);
        // One content's worth: the oldest goes.
        if (m_spareBuffers.size() > kMaxSpareBuffers)
        {
            evicted = m_spareBuffers.front();
            m_spareBuffers.erase(m_spareBuffers.begin());
        }
    }
    buffer = Buffer{};
    DestroyBuffer(evicted);
}

void VulkanRayScene::WriteSets()
{
    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        WriteSet(slot);
    }
}

void VulkanRayScene::Retire(std::function<void()> release)
{
    if (m_retire)
    {
        m_retire(std::move(release));
    }
    else
    {
        release();
    }
}

void VulkanRayScene::WriteSet(uint32_t slot)
{
    m_slotSetsGenerations.resize(m_frameCount, 0);
    m_slotSetsGenerations[slot] = m_setsGeneration;

    // A new binding set for the slot (NVRHI's are not written again); the old one goes at once, as the
    // slot's last frame has finished, which is when the native set used to be written over.
    nvrhi::BindingSetDesc desc;
    nvrhi::rt::AccelStructHandle topLevel;
    const std::array<nvrhi::IBuffer*, 5> buffers = {
        m_meshNodes.handle, m_meshTriangles.handle, m_instances[slot].handle, m_topNodes[slot].handle, m_materials};
    for (uint32_t binding = 0; binding < buffers.size(); ++binding)
    {
        desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(binding, buffers[binding]));
    }
    if (m_acceleration)
    {
        nvrhi::rt::AccelStructDesc topLevelDesc;
        topLevelDesc.isTopLevel = true;
        topLevelDesc.debugName = "Ray scene top level";
        // The item holds a raw pointer: the handle lives until the binding set holds its own reference.
        topLevel = m_nvrhiVulkanDevice->createHandleForNativeAccelStruct(m_acceleration->GetTopLevel(slot), nullptr, topLevelDesc);
        if (!topLevel)
        {
            throw std::runtime_error("Failed to name the ray scene's top level for NVRHI");
        }
        desc.bindings.push_back(nvrhi::BindingSetItem::RayTracingAccelStruct(5, topLevel));
        desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_meshGeometry.handle));
        desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_sourceTriangles.handle));
        for (uint32_t index = 0; index < m_samplerTable.size(); ++index)
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(kRaySamplerBinding, m_samplerTable[index]).setArrayElement(index));
        }
    }
    m_bindingSets[slot] = CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a ray scene binding set");
    if (m_nvrhiVulkanDevice != nullptr)
    {
        m_sets[slot] = ToNative<VkDescriptorSet>(m_bindingSets[slot]->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSet));
    }
}

void VulkanRayScene::CreateAverageSet()
{
    if (!m_averageLayout)
    {
        return;
    }
    nvrhi::BindingSetDesc desc;
    desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_materials));
    for (uint32_t index = 0; index < m_samplerTable.size(); ++index)
    {
        desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(1, m_samplerTable[index]).setArrayElement(index));
    }
    desc.bindings.push_back(nvrhi::BindingSetItem::PushConstants(0, sizeof(RayMaterialConstants)));
    m_averageSet = CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_averageLayout, "Failed to create the ray material binding set");
}
}
