#include "ray_scene.h"

#include "buffer.h"
#include "compute_pass_util.h"

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
};

// Must match the RAY_MATERIAL_* flags in shaders/vulkan/ray_tracing_common.glsl.
constexpr uint32_t kRayMaterialDoubleSided = 1u;
// Alpha tested: hit shading's textured coverage test applies (ray_hit_common.glsl).
constexpr uint32_t kRayMaterialAlphaMask = 2u;

// One mesh's buffers as hit shading reads them (RayMeshGeometry in ray_hit_common.glsl): the vertex
// and index buffers' device addresses.
struct RayMeshGeometry
{
    VkDeviceAddress vertices = 0;
    VkDeviceAddress indices = 0;
};
static_assert(sizeof(RayMeshGeometry) == 16, "RayMeshGeometry must match ray_hit_common.glsl");
// Hit shading reads vertices as floats at these offsets (RAY_VERTEX_* in ray_hit_common.glsl).
static_assert(sizeof(Vertex) == 17 * sizeof(float), "RAY_VERTEX_FLOATS in ray_hit_common.glsl must match Vertex");
static_assert(offsetof(Vertex, color) == 3 * sizeof(float) && offsetof(Vertex, texCoord) == 6 * sizeof(float) &&
                  offsetof(Vertex, normal) == 8 * sizeof(float) && offsetof(Vertex, texCoord1) == 15 * sizeof(float),
              "RAY_VERTEX_* offsets in ray_hit_common.glsl must match Vertex");

// Matches RayMaterial in ray_tracing_common.glsl: albedo and coverage, emission and flags.
constexpr VkDeviceSize kRayMaterialBytes = 32;

// Every buffer holds at least one element of the largest kind (an instance), so a descriptor always
// names something and the empty scene's dummy instance fits.
VkDeviceSize AtLeastOne(VkDeviceSize size)
{
    return std::max<VkDeviceSize>(size, sizeof(RayInstance));
}
}

VulkanRayScene::VulkanRayScene(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t frameCount,
    bool hardwareRayTracing,
    TextureDescriptorBinding defaultTexture)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_frameCount(frameCount),
      m_defaultTexture(defaultTexture)
{
    try
    {
        if (hardwareRayTracing)
        {
            m_acceleration = std::make_unique<VulkanRayAcceleration>(m_physicalDevice, m_device, m_frameCount);
        }
        std::vector<VkDescriptorType> types(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        std::vector<VkDescriptorPoolSize> poolSizes = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * m_frameCount}};
        if (m_acceleration)
        {
            types.push_back(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
            types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            poolSizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, m_frameCount});
        }
        // The lighting pass traces too (its local lights' shadows), from its fragment shader.
        m_setLayout = CreateComputeSetLayout(m_device, types, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        if (m_acceleration)
        {
            // The texture table: as many entries as the device lets one stage see, less a margin for
            // the frame set's own samplers; each content allocates what its slots need.
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
            const uint32_t limit = std::min(properties.limits.maxPerStageDescriptorSampledImages, properties.limits.maxDescriptorSetSampledImages);
            m_textureLimit = std::min<uint32_t>(limit > 256u ? limit - 256u : limit / 2u, 1u << 20);
            const VkDescriptorBindingFlags bindingFlags =
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
            flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
            flagsInfo.bindingCount = 1;
            flagsInfo.pBindingFlags = &bindingFlags;
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = m_textureLimit;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.pNext = &flagsInfo;
            layoutInfo.bindingCount = 1;
            layoutInfo.pBindings = &binding;
            CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_textureSetLayout), "Failed to create the ray texture table layout");
        }

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = m_frameCount;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the ray scene descriptor pool");
        m_sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, m_frameCount);

        // Placeholder buffers, so the sets are valid before the first build.
        m_meshNodes = CreateBuffer(AtLeastOne(0));
        m_meshTriangles = CreateBuffer(AtLeastOne(0));
        m_meshGeometry = CreateBuffer(AtLeastOne(0), true);
        m_sourceTriangles = CreateBuffer(AtLeastOne(0), true);
        m_materials = CreateBuffer(AtLeastOne(0), true);
        for (uint32_t slot = 0; slot < m_frameCount; ++slot)
        {
            m_instances.push_back(CreateBuffer(AtLeastOne(0), true));
            m_topNodes.push_back(CreateBuffer(AtLeastOne(0), true));
        }
        WriteSets();
        CreateMaterialPipeline(pipelineCache);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
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
    DestroyHandles();
}

namespace
{
constexpr uint32_t kMaterialSetsPerPool = 1024;
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
        m_staleBuilds.push_back(std::move(m_pendingBuild));
    }
    m_ready = false;

    // Slots that lost their submesh give their sets back; a slot placed again below gets a new one.
    for (const uint32_t index : released)
    {
        if (index < m_materialSlots.size() && m_materialSlots[index].set != VK_NULL_HANDLE)
        {
            m_materialPools->Free(VulkanDescriptorPoolList::Allocation{m_materialSlots[index].set, m_materialSlots[index].pool});
            m_materialSlots[index] = MaterialSlot{};
        }
    }

    // More slots than the buffer holds: a larger buffer, which every set must name and every
    // material be averaged into again.
    const uint32_t capacity = std::max(slotCapacity, 1u);
    std::vector<uint32_t> rewrite;
    if (capacity > m_materialCapacity)
    {
        DestroyBuffer(m_materials);
        m_materials = CreateBuffer(kRayMaterialBytes * capacity, true);
        m_materialCapacity = capacity;
        m_materialSlots.resize(capacity);
        for (uint32_t index = 0; index < capacity; ++index)
        {
            if (m_materialSlots[index].set != VK_NULL_HANDLE)
            {
                rewrite.push_back(index);
            }
        }
    }
    for (const RayMaterialSource& source : placed)
    {
        if (source.slot >= m_materialCapacity)
        {
            throw std::runtime_error("A ray material slot lies past the slot capacity");
        }
        MaterialSlot& slot = m_materialSlots[source.slot];
        if (slot.set == VK_NULL_HANDLE)
        {
            if (!m_materialPools)
            {
                m_materialPools = std::make_unique<VulkanDescriptorPoolList>(
                    m_device,
                    m_materialSetLayout,
                    std::vector<VkDescriptorPoolSize>{
                        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
                    kMaterialSetsPerPool);
            }
            const VulkanDescriptorPoolList::Allocation allocation = m_materialPools->Allocate();
            slot.set = allocation.set;
            slot.pool = allocation.pool;
        }
        slot.source = source;
        rewrite.push_back(source.slot);
    }
    const VkDescriptorBufferInfo output{m_materials.buffer, 0, VK_WHOLE_SIZE};
    for (const uint32_t index : rewrite)
    {
        MaterialSlot& slot = m_materialSlots[index];
        const VkDescriptorImageInfo baseColor{slot.source.baseColor.sampler, slot.source.baseColor.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo emissive{slot.source.emissive.sampler, slot.source.emissive.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = ImageWrite(slot.set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &baseColor);
        writes[1] = ImageWrite(slot.set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &emissive);
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = slot.set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &output;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        m_dirtyMaterialSlots.push_back(index);
    }

    // The texture table: a larger one when the slots outgrew it, with every slot written; otherwise
    // the placed slots get their textures and the released ones the default.
    if (m_textureSetLayout != VK_NULL_HANDLE)
    {
        std::vector<VkDescriptorImageInfo> infos;
        std::vector<VkWriteDescriptorSet> writes;
        if (capacity > m_textureCapacity)
        {
            if (static_cast<uint64_t>(capacity) * kRayTexturesPerSlot > m_textureLimit)
            {
                throw std::runtime_error("The ray texture table outgrew the device's sampled image limit");
            }
            if (m_texturePool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(m_device, m_texturePool, nullptr);
                m_texturePool = VK_NULL_HANDLE;
                m_textureSet = VK_NULL_HANDLE;
            }
            // Room to grow, so a streamed map does not reallocate it at every new cell.
            m_textureCapacity = std::min<uint32_t>(std::max(capacity + capacity / 2u, 1024u), m_textureLimit / kRayTexturesPerSlot);
            const uint32_t count = m_textureCapacity * kRayTexturesPerSlot;
            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_texturePool), "Failed to create the ray texture table pool");
            VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
            variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
            variableInfo.descriptorSetCount = 1;
            variableInfo.pDescriptorCounts = &count;
            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.pNext = &variableInfo;
            allocateInfo.descriptorPool = m_texturePool;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &m_textureSetLayout;
            CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, &m_textureSet), "Failed to allocate the ray texture table");
            infos.reserve(static_cast<size_t>(count));
            for (uint32_t index = 0; index < m_textureCapacity; ++index)
            {
                const bool held = index < m_materialSlots.size() && m_materialSlots[index].set != VK_NULL_HANDLE;
                WriteTextureSlot(index, held ? &m_materialSlots[index].source : nullptr, infos, writes);
            }
        }
        else
        {
            infos.reserve((placed.size() + released.size()) * kRayTexturesPerSlot);
            for (const uint32_t index : released)
            {
                if (index < m_textureCapacity)
                {
                    WriteTextureSlot(index, nullptr, infos, writes);
                }
            }
            for (const RayMaterialSource& source : placed)
            {
                WriteTextureSlot(source.slot, &source, infos, writes);
            }
        }
        if (!writes.empty())
        {
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    m_submeshes = std::move(submeshes);
    WriteSets();

    // What the hardware may treat as opaque: the materials whose coverage the averaging sets to 1.
    m_opaqueMaterials.assign(m_materialCapacity, 0u);
    for (uint32_t index = 0; index < m_materialCapacity; ++index)
    {
        const MaterialSlot& slot = m_materialSlots[index];
        const GpuMaterialData& material = slot.source.material;
        const bool transmits = (material.shadingModel[0] & kShadingFlagTransmission) != 0u && material.transmissionFactors[0] > 0.0f;
        m_opaqueMaterials[index] = slot.set != VK_NULL_HANDLE && slot.source.alphaMode == MaterialAlphaMode::Opaque && !transmits ? 1u : 0u;
    }

    // The hierarchies, on a worker: only meshes not built before cost anything. The previous
    // content's hierarchies go in with it and come back out pruned to what this content uses.
    std::vector<std::shared_ptr<const MeshData>> meshes;
    std::vector<std::shared_ptr<const VulkanBuffer>> buffers;
    std::vector<uint8_t> blend;
    std::vector<uint32_t> slots;
    meshes.reserve(m_submeshes.size());
    for (const RaySceneSubmesh& submesh : m_submeshes)
    {
        meshes.push_back(submesh.mesh);
        buffers.push_back(submesh.buffer);
        blend.push_back(submesh.blend ? 1u : 0u);
        slots.push_back(submesh.slot);
    }
    // The task uses this only to make its buffers, which the destructor waits for.
    m_pendingBuild = RunAsync(
        TaskPriority::Medium,
        [this, meshes = std::move(meshes), buffers = std::move(buffers), blend = std::move(blend), slots = std::move(slots), models = std::move(models), cache = m_buildCache]() mutable
        {
            const auto start = std::chrono::steady_clock::now();
            Build build;
            build.blend = std::move(blend);
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
            // Below the frame's own parallel loops, which the task system runs first.
            TaskSystem::ParallelFor(
                static_cast<uint32_t>(distinct.size()),
                1,
                [&](uint32_t begin, uint32_t end)
                {
                    for (uint32_t index = begin; index < end; ++index)
                    {
                        if (bvhs[index])
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
                    }
                },
                TaskPriority::Medium);

            for (size_t index = 0; index < distinct.size(); ++index)
            {
                AppendMesh(build.scene, *bvhs[index]);
            }
            // The bottom-level acceleration structures of meshes that have none, made here and built on
            // the GPU when this content installs.
            if (m_acceleration)
            {
                build.blas = m_acceleration->Prepare(distinct, bvhs);

                // Hit shading's view of the meshes: each one's buffer addresses, and each leaf
                // triangle's index in its index list, laid out as the triangles are.
                build.meshGeometry = CreateBuffer(AtLeastOne(sizeof(RayMeshGeometry) * distinct.size()), true);
                auto* geometry = static_cast<RayMeshGeometry*>(build.meshGeometry.mapped);
                size_t triangleCount = 0;
                for (size_t index = 0; index < distinct.size(); ++index)
                {
                    const std::shared_ptr<const VulkanBuffer>& buffer = build.meshBuffers[index];
                    geometry[index] = buffer ? RayMeshGeometry{buffer->GetVertexAddress(), buffer->GetIndexAddress()} : RayMeshGeometry{};
                    triangleCount += bvhs[index]->sourceTriangles.size();
                }
                build.sourceTriangles = CreateBuffer(AtLeastOne(sizeof(uint32_t) * triangleCount), true);
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
            // The hierarchies into GPU buffers here rather than at install: hundreds of megabytes on a map.
            build.meshNodeCount = build.scene.meshNodes.size();
            build.meshTriangleCount = build.scene.meshTriangles.size();
            build.meshNodes = CreateBuffer(AtLeastOne(sizeof(BvhNode) * build.meshNodeCount));
            build.meshTriangles = CreateBuffer(AtLeastOne(sizeof(BvhTriangle) * build.meshTriangleCount));
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
                    build.submeshMeshes[index], models[index], slots[index], build.blend[index] != 0 ? kRayInstanceSkip : 0u});
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

bool VulkanRayScene::HasFinishedBuild() const
{
    return m_pendingBuild.valid() && m_pendingBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void VulkanRayScene::InstallBuild()
{
    Build build = m_pendingBuild.get();
    DropFinishedStaleBuilds();
    m_submeshMeshes = std::move(build.submeshMeshes);
    m_installedBlend = std::move(build.blend);
    // The old content goes off the frame's thread, as a superseded build does.
    Build previous;
    previous.scene = std::move(m_scene);
    previous.topLevel = std::move(m_topLevel);
    previous.meshNodes = m_meshNodes;
    previous.meshTriangles = m_meshTriangles;
    previous.meshBuffers = std::move(m_meshBuffers);
    // Without hardware ray tracing the placeholders stay.
    if (build.meshGeometry.buffer != VK_NULL_HANDLE)
    {
        previous.meshGeometry = m_meshGeometry;
        previous.sourceTriangles = m_sourceTriangles;
    }
    DiscardBuild(std::move(previous));
    m_scene = std::move(build.scene);
    m_topLevel = std::move(build.topLevel);
    m_meshNodes = build.meshNodes;
    m_meshTriangles = build.meshTriangles;
    if (build.meshGeometry.buffer != VK_NULL_HANDLE)
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

    const size_t instanceCount = m_submeshMeshes.size();
    if (m_acceleration)
    {
        // The bottom levels only the old content held are freed off the frame's thread too.
        Build released;
        released.blas = m_acceleration->Install(std::move(build.blas), m_scene.meshes, IncrementalTopLevel::MaxInstances(instanceCount));
        DiscardBuild(std::move(released));
    }
    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        DestroyBuffer(m_instances[slot]);
        DestroyBuffer(m_topNodes[slot]);
        m_instances[slot] = CreateBuffer(AtLeastOne(sizeof(RayInstance) * IncrementalTopLevel::MaxInstances(instanceCount)), true);
        m_topNodes[slot] = CreateBuffer(AtLeastOne(sizeof(BvhNode) * IncrementalTopLevel::MaxNodes(instanceCount)), true);
    }
    // New buffers, which every frame slot copies the worker's top level into.
    m_slotGenerations.assign(m_frameCount, 0);
    ++m_topLevelGeneration;
    WriteSets();
    m_ready = true;
}

void VulkanRayScene::UpdateInstances(uint32_t frameSlot, std::span<const glm::mat4> models, std::span<const uint8_t> movingInstances)
{
    if (!m_ready || models.size() != m_submeshMeshes.size())
    {
        return;
    }
    std::vector<RayInstanceInput> inputs;
    inputs.reserve(models.size());
    for (uint32_t index = 0; index < static_cast<uint32_t>(models.size()); ++index)
    {
        // Blend surfaces are decals laid over others (New Sponza's dirt) or glass: thin layers that
        // add little to the light between surfaces, and that a coverage decision per ray turns into
        // noise on everything they lie on. Rays pass through them.
        const bool blend = index < m_installedBlend.size() && m_installedBlend[index] != 0;
        const bool moving = index < movingInstances.size() && movingInstances[index] != 0;
        const uint32_t flags = blend ? kRayInstanceSkip : moving ? kRayInstanceDynamic : 0u;
        inputs.push_back(RayInstanceInput{m_submeshMeshes[index], models[index], m_submeshes[index].slot, flags});
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

void VulkanRayScene::Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, bool hardwareRays)
{
    if (!m_dirtyMaterialSlots.empty() && m_materialPipeline != VK_NULL_HANDLE)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_materialPipeline);
        for (const uint32_t index : m_dirtyMaterialSlots)
        {
            // A slot that lost its submesh again before this record has nothing to average.
            if (index >= m_materialSlots.size() || m_materialSlots[index].set == VK_NULL_HANDLE)
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
                (submesh.doubleSided ? kRayMaterialDoubleSided : 0u) | (submesh.alphaMode == MaterialAlphaMode::Mask ? kRayMaterialAlphaMask : 0u),
                transmissionBits);
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_materialPipelineLayout, 0, 1, &m_materialSlots[index].set, 0, nullptr);
            vkCmdPushConstants(commandBuffer, m_materialPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(commandBuffer, 1, 1, 1);
        }
        m_dirtyMaterialSlots.clear();
    }

    // The materials just written, and the host writes of the frame's instances, before any trace.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // The acceleration structures read only what the worker and UpdateInstances wrote from the host,
    // which a submission makes visible by itself.
    if (m_acceleration)
    {
        m_acceleration->Record(commandBuffer, frameSlot, hardwareRays && m_ready);
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
            DestroyBuffer(build.meshNodes);
            DestroyBuffer(build.meshTriangles);
            DestroyBuffer(build.meshGeometry);
            DestroyBuffer(build.sourceTriangles);
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
    return m_setLayout;
}

VkDescriptorSet VulkanRayScene::GetSet(uint32_t frameSlot) const
{
    return m_sets[frameSlot];
}

VkDescriptorSetLayout VulkanRayScene::GetTextureSetLayout() const
{
    return m_textureSetLayout;
}

VkDescriptorSet VulkanRayScene::GetTextureSet() const
{
    return m_textureSet;
}

void VulkanRayScene::WriteTextureSlot(
    uint32_t slot,
    const RayMaterialSource* source,
    std::vector<VkDescriptorImageInfo>& infos,
    std::vector<VkWriteDescriptorSet>& writes) const
{
    // The order of kRayTexturesPerSlot (RAY_TEXTURE_* in ray_hit_common.glsl).
    using Bindings = std::array<TextureDescriptorBinding, kRayTexturesPerSlot>;
    const Bindings bindings = source != nullptr
                                  ? Bindings{source->baseColor, source->metallic, source->roughness, source->emissive}
                                  : Bindings{m_defaultTexture, m_defaultTexture, m_defaultTexture, m_defaultTexture};
    // infos was reserved for every write, so the pointers taken below stay valid.
    const size_t first = infos.size();
    for (uint32_t index = 0; index < kRayTexturesPerSlot; ++index)
    {
        const TextureDescriptorBinding& binding = bindings[index].imageView != VK_NULL_HANDLE ? bindings[index] : m_defaultTexture;
        infos.push_back(VkDescriptorImageInfo{binding.sampler, binding.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_textureSet;
    write.dstBinding = 0;
    write.dstArrayElement = slot * kRayTexturesPerSlot;
    write.descriptorCount = kRayTexturesPerSlot;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &infos[first];
    writes.push_back(write);
}

size_t VulkanRayScene::GetSubmeshCount() const
{
    return m_submeshMeshes.size();
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
    if (!m_ready || m_materials.mapped == nullptr)
    {
        return materials;
    }
    const auto* values = static_cast<const glm::vec4*>(m_materials.mapped);
    materials.resize(m_materialCapacity);
    for (size_t index = 0; index < materials.size(); ++index)
    {
        const glm::vec4 albedoCoverage = values[index * 2];
        const glm::vec4 emissionFlags = values[index * 2 + 1];
        uint32_t flags = 0;
        std::memcpy(&flags, &emissionFlags.w, sizeof(flags));
        materials[index].albedo = glm::vec3(albedoCoverage);
        materials[index].coverage = albedoCoverage.w;
        materials[index].emission = glm::vec3(emissionFlags);
        materials[index].doubleSided = (flags & kRayMaterialDoubleSided) != 0u;
    }
    return materials;
}

VulkanRayScene::Buffer VulkanRayScene::CreateBuffer(VkDeviceSize size, bool nearGpu) const
{
    Buffer result{};
    result.size = size;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &result.buffer), "Failed to create a ray scene buffer");
    try
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, result.buffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        // Host visible: the CPU writes the hierarchies and every frame's instances straight in. On
        // this Mac's unified memory that is also where the GPU reads fastest. On a discrete GPU with
        // resizable BAR the buffers every hit reads go to video memory, which the CPU writes as well;
        // where there is none, or no room, they stay in system memory.
        constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkResult allocated = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (nearGpu)
        {
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
            for (uint32_t type = 0; type < memory.memoryTypeCount && allocated != VK_SUCCESS; ++type)
            {
                const VkMemoryPropertyFlags flags = memory.memoryTypes[type].propertyFlags;
                // A full-size BAR heap only: a 256 MiB window is better left to the driver.
                const VkDeviceSize heapSize = memory.memoryHeaps[memory.memoryTypes[type].heapIndex].size;
                if ((requirements.memoryTypeBits & (1u << type)) != 0 && (flags & (kHostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ==
                                                                          (kHostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                    heapSize > (VkDeviceSize{1} << 30))
                {
                    allocateInfo.memoryTypeIndex = type;
                    allocated = vkAllocateMemory(m_device, &allocateInfo, nullptr, &result.memory);
                }
            }
        }
        if (allocated != VK_SUCCESS)
        {
            result.memory = VK_NULL_HANDLE;
            allocateInfo.memoryTypeIndex = FindMemoryType(m_physicalDevice, requirements.memoryTypeBits, kHostVisible);
            CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &result.memory), "Failed to allocate a ray scene buffer");
        }
        CheckVulkan(vkBindBufferMemory(m_device, result.buffer, result.memory, 0), "Failed to bind a ray scene buffer");
        CheckVulkan(vkMapMemory(m_device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped), "Failed to map a ray scene buffer");
    }
    catch (...)
    {
        Buffer partial = result;
        DestroyBuffer(partial);
        throw;
    }
    return result;
}

void VulkanRayScene::DestroyBuffer(Buffer& buffer) const
{
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, buffer.memory, nullptr);
    }
    buffer = Buffer{};
}

void VulkanRayScene::WriteSets()
{
    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        const std::array<VkDescriptorBufferInfo, 5> infos = {
            VkDescriptorBufferInfo{m_meshNodes.buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{m_meshTriangles.buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{m_instances[slot].buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{m_topNodes[slot].buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{m_materials.buffer, 0, VK_WHOLE_SIZE}};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = m_sets[slot];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &infos[binding];
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        if (m_acceleration)
        {
            const VkAccelerationStructureKHR topLevel = m_acceleration->GetTopLevel(slot);
            VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{};
            accelerationInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            accelerationInfo.accelerationStructureCount = 1;
            accelerationInfo.pAccelerationStructures = &topLevel;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.pNext = &accelerationInfo;
            write.dstSet = m_sets[slot];
            write.dstBinding = 5;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

            const std::array<VkDescriptorBufferInfo, 2> hitInfos = {
                VkDescriptorBufferInfo{m_meshGeometry.buffer, 0, VK_WHOLE_SIZE},
                VkDescriptorBufferInfo{m_sourceTriangles.buffer, 0, VK_WHOLE_SIZE}};
            std::array<VkWriteDescriptorSet, 2> hitWrites{};
            for (uint32_t index = 0; index < hitWrites.size(); ++index)
            {
                hitWrites[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                hitWrites[index].dstSet = m_sets[slot];
                hitWrites[index].dstBinding = 6 + index;
                hitWrites[index].descriptorCount = 1;
                hitWrites[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                hitWrites[index].pBufferInfo = &hitInfos[index];
            }
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(hitWrites.size()), hitWrites.data(), 0, nullptr);
        }
    }
}

void VulkanRayScene::CreateMaterialPipeline(VkPipelineCache pipelineCache)
{
    constexpr std::array<VkDescriptorType, 3> kTypes = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    m_materialSetLayout = CreateComputeSetLayout(m_device, kTypes);

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(RayMaterialConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_materialSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_materialPipelineLayout), "Failed to create the ray material pipeline layout");
    m_materialPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_materialPipelineLayout, "ray_material_average.comp.spv");
}

void VulkanRayScene::DestroyHandles()
{
    if (m_materialPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_materialPipeline, nullptr);
        m_materialPipeline = VK_NULL_HANDLE;
    }
    if (m_materialPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_materialPipelineLayout, nullptr);
        m_materialPipelineLayout = VK_NULL_HANDLE;
    }
    m_materialPools.reset();
    m_materialSlots.clear();
    if (m_materialSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_materialSetLayout, nullptr);
        m_materialSetLayout = VK_NULL_HANDLE;
    }
    DestroyBuffer(m_meshNodes);
    DestroyBuffer(m_meshTriangles);
    DestroyBuffer(m_meshGeometry);
    DestroyBuffer(m_sourceTriangles);
    m_meshBuffers.clear();
    DestroyBuffer(m_materials);
    if (m_texturePool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_texturePool, nullptr);
        m_texturePool = VK_NULL_HANDLE;
        m_textureSet = VK_NULL_HANDLE;
    }
    if (m_textureSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_textureSetLayout, nullptr);
        m_textureSetLayout = VK_NULL_HANDLE;
    }
    for (Buffer& buffer : m_instances)
    {
        DestroyBuffer(buffer);
    }
    for (Buffer& buffer : m_topNodes)
    {
        DestroyBuffer(buffer);
    }
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
