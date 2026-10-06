#include "ray_acceleration.h"

#include "compute_pass_util.h"

#include <engine/core/log/log.h>
#include <engine/core/threading/task_system.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace me
{

// The extension's entry points: the loader exports core functions only.
struct RayTracingFunctions
{
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getDeviceAddress = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
};

struct RayBlas::VertexBatch
{
    ~VertexBatch()
    {
        if (buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, memory, nullptr);
        }
    }

    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};

RayBlas::~RayBlas()
{
    if (handle != VK_NULL_HANDLE)
    {
        functions->destroyAccelerationStructure(device, handle, nullptr);
    }
    if (buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, buffer, nullptr);
    }
    VulkanMemoryPool::Free(device, memory);
}

namespace
{
template <typename Function>
Function LoadDeviceFunction(VkDevice device, const char* name)
{
    const auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, name));
    if (function == nullptr)
    {
        throw std::runtime_error(std::string("Missing ray tracing entry point ") + name);
    }
    return function;
}

VkDeviceAddress BufferAddress(VkDevice device, VkBuffer buffer)
{
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

VkDeviceAddress AlignUp(VkDeviceAddress value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Each triangle as its three vertices, the hierarchy's v0 and v0 plus each edge.
constexpr VkDeviceSize kTriangleVertexBytes = sizeof(float) * 9;

VkAccelerationStructureGeometryKHR TriangleGeometry(VkDeviceAddress vertices, uint32_t triangleCount)
{
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Not opaque: the instances say whether their material stops every ray (UpdateTopLevel); each
    // candidate is tested once, as the compute walk does.
    geometry.flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertices;
    triangles.vertexStride = sizeof(float) * 3;
    triangles.maxVertex = triangleCount * 3 - 1;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    return geometry;
}

VkAccelerationStructureGeometryKHR InstanceGeometry(VkDeviceAddress instances)
{
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = instances;
    return geometry;
}

void AccelerationBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// An acceleration structure's address must be a multiple of 256 bytes; its offset in its buffer is 0,
// so the buffer's memory must be.
constexpr VkDeviceSize kStructureAlignment = 256;

// One bottom-level build batch's scratch at most, unless a single mesh needs more: the GTA map's
// first content builds some 6 million triangles at once.
constexpr VkDeviceSize kScratchBudget = VkDeviceSize{256} << 20;
}

VulkanRayAcceleration::VulkanRayAcceleration(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t frameCount)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_frameCount(frameCount)
{
    auto functions = std::make_shared<RayTracingFunctions>();
    functions->createAccelerationStructure = LoadDeviceFunction<PFN_vkCreateAccelerationStructureKHR>(device, "vkCreateAccelerationStructureKHR");
    functions->destroyAccelerationStructure = LoadDeviceFunction<PFN_vkDestroyAccelerationStructureKHR>(device, "vkDestroyAccelerationStructureKHR");
    functions->getBuildSizes = LoadDeviceFunction<PFN_vkGetAccelerationStructureBuildSizesKHR>(device, "vkGetAccelerationStructureBuildSizesKHR");
    functions->getDeviceAddress = LoadDeviceFunction<PFN_vkGetAccelerationStructureDeviceAddressKHR>(device, "vkGetAccelerationStructureDeviceAddressKHR");
    functions->cmdBuild = LoadDeviceFunction<PFN_vkCmdBuildAccelerationStructuresKHR>(device, "vkCmdBuildAccelerationStructuresKHR");
    m_functions = std::move(functions);

    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{};
    accelerationProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &accelerationProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    m_scratchAlignment = std::max<VkDeviceSize>(accelerationProperties.minAccelerationStructureScratchOffsetAlignment, 1);

    m_retired.resize(m_frameCount);
    m_topLevels.resize(m_frameCount);
    try
    {
        // One instance each until content installs, so the descriptor sets name a structure.
        for (TopLevel& topLevel : m_topLevels)
        {
            CreateTopLevel(topLevel, 1);
        }
    }
    catch (...)
    {
        for (TopLevel& topLevel : m_topLevels)
        {
            DestroyTopLevel(topLevel);
        }
        throw;
    }
}

VulkanRayAcceleration::~VulkanRayAcceleration()
{
    for (TopLevel& topLevel : m_topLevels)
    {
        DestroyTopLevel(topLevel);
    }
    for (Retired& retired : m_retired)
    {
        for (Buffer& buffer : retired.buffers)
        {
            DestroyBuffer(buffer);
        }
    }
}

std::vector<std::shared_ptr<RayBlas>> VulkanRayAcceleration::Prepare(
    std::span<const std::shared_ptr<const MeshData>> meshes,
    std::span<const std::shared_ptr<const MeshBvh>> bvhs) const
{
    std::vector<std::shared_ptr<RayBlas>> result(meshes.size());
    // The meshes a live bottom level already covers keep it; the rest are made below. A mesh another
    // build made but has not installed yet counts as live: whichever content installs first builds it.
    std::vector<uint32_t> fresh;
    {
        std::lock_guard lock(m_cache->mutex);
        for (uint32_t index = 0; index < meshes.size(); ++index)
        {
            if (!meshes[index] || !bvhs[index] || bvhs[index]->triangles.empty())
            {
                continue;
            }
            const auto cached = m_cache->meshes.find(meshes[index].get());
            if (cached != m_cache->meshes.end() && cached->second.mesh.lock() == meshes[index])
            {
                result[index] = cached->second.blas.lock();
            }
            if (!result[index])
            {
                fresh.push_back(index);
            }
        }
    }
    if (fresh.empty())
    {
        return result;
    }

    // The new meshes' triangles in one host-visible batch: one allocation however many meshes.
    std::vector<VkDeviceSize> offsets(fresh.size());
    VkDeviceSize total = 0;
    for (size_t index = 0; index < fresh.size(); ++index)
    {
        offsets[index] = total;
        total += kTriangleVertexBytes * bvhs[fresh[index]]->triangles.size();
    }
    Buffer batchBuffer = CreateBuffer(
        total,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        true);
    auto batch = std::make_shared<RayBlas::VertexBatch>();
    batch->device = m_device;
    batch->buffer = batchBuffer.buffer;
    batch->memory = batchBuffer.memory;
    batch->address = batchBuffer.address;

    std::vector<std::shared_ptr<RayBlas>> made(fresh.size());
    TaskSystem::ParallelFor(
        static_cast<uint32_t>(fresh.size()),
        16,
        [&](uint32_t begin, uint32_t end)
        {
            for (uint32_t index = begin; index < end; ++index)
            {
                const MeshBvh& bvh = *bvhs[fresh[index]];
                auto* vertices = reinterpret_cast<float*>(static_cast<std::byte*>(batchBuffer.mapped) + offsets[index]);
                for (const BvhTriangle& triangle : bvh.triangles)
                {
                    const glm::vec3 v0(triangle.v0);
                    const glm::vec3 v1 = v0 + glm::vec3(triangle.e1);
                    const glm::vec3 v2 = v0 + glm::vec3(triangle.e2);
                    const float packed[9] = {v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z};
                    std::memcpy(vertices, packed, sizeof(packed));
                    vertices += 9;
                }

                auto blas = std::make_shared<RayBlas>();
                blas->functions = m_functions;
                blas->device = m_device;
                blas->vertices = batch;
                blas->vertexOffset = offsets[index];
                blas->triangleCount = static_cast<uint32_t>(bvh.triangles.size());

                const VkAccelerationStructureGeometryKHR geometry = TriangleGeometry(0, blas->triangleCount);
                VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
                buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
                buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
                buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                buildInfo.geometryCount = 1;
                buildInfo.pGeometries = &geometry;
                VkAccelerationStructureBuildSizesInfoKHR sizes{};
                sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
                m_functions->getBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &blas->triangleCount, &sizes);
                blas->scratchSize = sizes.buildScratchSize;

                VkBufferCreateInfo bufferInfo{};
                bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufferInfo.size = sizes.accelerationStructureSize;
                bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &blas->buffer), "Failed to create a bottom-level acceleration structure buffer");
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(m_device, blas->buffer, &requirements);
                requirements.alignment = std::max<VkDeviceSize>(requirements.alignment, kStructureAlignment);
                blas->memory = VulkanMemoryPool::Allocate(
                    m_physicalDevice, m_device, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VulkanMemoryPool::Resource::AddressableBuffer);
                CheckVulkan(vkBindBufferMemory(m_device, blas->buffer, blas->memory.memory, blas->memory.offset), "Failed to bind a bottom-level acceleration structure buffer");

                VkAccelerationStructureCreateInfoKHR createInfo{};
                createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
                createInfo.buffer = blas->buffer;
                createInfo.size = sizes.accelerationStructureSize;
                createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                CheckVulkan(
                    m_functions->createAccelerationStructure(m_device, &createInfo, nullptr, &blas->handle),
                    "Failed to create a bottom-level acceleration structure");
                VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
                addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                addressInfo.accelerationStructure = blas->handle;
                blas->address = m_functions->getDeviceAddress(m_device, &addressInfo);
                made[index] = std::move(blas);
            }
        },
        TaskPriority::Medium);

    std::lock_guard lock(m_cache->mutex);
    for (size_t index = 0; index < fresh.size(); ++index)
    {
        const uint32_t mesh = fresh[index];
        // Another build may have made one meanwhile; either works, the later one wins the cache.
        m_cache->meshes[meshes[mesh].get()] = BuiltMesh{meshes[mesh], made[index]};
        result[mesh] = std::move(made[index]);
    }
    std::erase_if(m_cache->meshes, [](const auto& entry)
                  {
                      return entry.second.mesh.expired() || entry.second.blas.expired();
                  });
    return result;
}

std::vector<std::shared_ptr<RayBlas>> VulkanRayAcceleration::Install(
    std::vector<std::shared_ptr<RayBlas>> meshBlas,
    std::span<const RayMeshRange> meshes,
    size_t instanceCapacity)
{
    std::vector<std::shared_ptr<RayBlas>> previous = std::move(m_meshBlas);
    m_meshBlas = std::move(meshBlas);
    m_meshBlas.resize(meshes.size());
    // Pending builds of the content this replaces that this one does not hold go with it.
    m_pending.clear();
    m_addressByNodeOffset.clear();
    for (size_t index = 0; index < meshes.size(); ++index)
    {
        const std::shared_ptr<RayBlas>& blas = m_meshBlas[index];
        if (!blas || meshes[index].nodeCount == 0)
        {
            continue;
        }
        m_addressByNodeOffset[meshes[index].nodeOffset] = blas->address;
        // Two meshes never share one, but a mesh made by a build that never installed may be pending
        // here for the first time.
        if (!blas->built)
        {
            m_pending.push_back(blas);
        }
    }

    // Sized for this content exactly: a map's capacity is large, and a small scene after it should not
    // keep it.
    const size_t capacity = std::max<size_t>(instanceCapacity, 1);
    for (TopLevel& topLevel : m_topLevels)
    {
        if (topLevel.capacity != capacity)
        {
            DestroyTopLevel(topLevel);
            CreateTopLevel(topLevel, capacity);
        }
        topLevel.generation = 0;
        topLevel.count = 0;
        topLevel.dirty = true;
        // New bottom-level addresses and materials: every instance is written again.
        topLevel.written.clear();
    }
    return previous;
}

void VulkanRayAcceleration::UpdateTopLevel(uint32_t frameSlot, const RayScene& scene, uint64_t generation, std::span<const uint8_t> opaqueMaterials)
{
    Retired& retired = m_retired[frameSlot];
    for (Buffer& buffer : retired.buffers)
    {
        DestroyBuffer(buffer);
    }
    retired.buffers.clear();
    retired.batches.clear();

    TopLevel& topLevel = m_topLevels[frameSlot];
    if (topLevel.generation == generation)
    {
        return;
    }
    const size_t count = std::min(scene.instances.size(), topLevel.capacity);
    const size_t known = std::min(topLevel.written.size(), count);
    topLevel.written.resize(count);
    auto* instances = static_cast<VkAccelerationStructureInstanceKHR*>(topLevel.instances.mapped);
    // The custom index carries the instance's leaf index, 24 bits of it.
    constexpr size_t kMaxCustomIndex = (size_t{1} << 24) - 1;
    TaskSystem::ParallelFor(
        static_cast<uint32_t>(count),
        4096,
        [&](uint32_t begin, uint32_t end)
        {
            for (uint32_t index = begin; index < end; ++index)
            {
                const RayInstance& source = scene.instances[index];
                if (index < known && std::memcmp(&topLevel.written[index], &source, sizeof(RayInstance)) == 0)
                {
                    continue;
                }
                topLevel.written[index] = source;
                VkAccelerationStructureInstanceKHR instance{};
                const auto address = m_addressByNodeOffset.find(source.data.x);
                const bool skipped = (source.data.w & kRayInstanceSkip) != 0u || address == m_addressByNodeOffset.end() ||
                                     index > kMaxCustomIndex;
                if (skipped)
                {
                    // Inactive: no structure, and no ray mask matches it.
                    instances[index] = instance;
                    continue;
                }
                // worldToObject's rows, back to the model matrix's top three rows.
                glm::mat4 worldToObject(1.0f);
                for (int row = 0; row < 3; ++row)
                {
                    for (int column = 0; column < 4; ++column)
                    {
                        worldToObject[column][row] = source.worldToObject[row][column];
                    }
                }
                const glm::mat4 objectToWorld = glm::affineInverse(worldToObject);
                for (int row = 0; row < 3; ++row)
                {
                    for (int column = 0; column < 4; ++column)
                    {
                        instance.transform.matrix[row][column] = objectToWorld[column][row];
                    }
                }
                instance.instanceCustomIndex = index;
                instance.mask = 0xFF;
                const uint32_t material = source.data.z;
                const bool opaque = material < opaqueMaterials.size() && opaqueMaterials[material] != 0u;
                instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                                 (opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
                instance.accelerationStructureReference = address->second;
                instances[index] = instance;
            }
        },
        TaskPriority::High);
    topLevel.count = static_cast<uint32_t>(count);
    topLevel.generation = generation;
    topLevel.dirty = true;
}

void VulkanRayAcceleration::Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, bool buildTopLevel)
{
    const bool builtBottom = !m_pending.empty();
    RecordBottomLevels(commandBuffer, frameSlot);

    TopLevel& topLevel = m_topLevels[frameSlot];
    if (buildTopLevel && topLevel.dirty)
    {
        if (builtBottom)
        {
            AccelerationBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        }
        const VkAccelerationStructureGeometryKHR geometry = InstanceGeometry(topLevel.instances.address);
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.dstAccelerationStructure = topLevel.handle;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;
        buildInfo.scratchData.deviceAddress = AlignUp(topLevel.scratch.address, m_scratchAlignment);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = topLevel.count;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        m_functions->cmdBuild(commandBuffer, 1, &buildInfo, &ranges);
        topLevel.dirty = false;
    }
    else if (!builtBottom)
    {
        return;
    }
    // Ray queries this frame, and the top-level builds of later frames that read the bottom levels.
    AccelerationBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

void VulkanRayAcceleration::RecordBottomLevels(VkCommandBuffer commandBuffer, uint32_t frameSlot)
{
    if (m_pending.empty())
    {
        return;
    }
    VkDeviceSize largest = 0;
    VkDeviceSize total = 0;
    VkDeviceSize storage = 0;
    size_t triangles = 0;
    for (const std::shared_ptr<RayBlas>& blas : m_pending)
    {
        const VkDeviceSize aligned = AlignUp(blas->scratchSize, m_scratchAlignment);
        largest = std::max(largest, aligned);
        total += aligned;
        storage += blas->memory.size;
        triangles += blas->triangleCount;
    }
    // One scratch for every batch, the batches apart by barriers; it goes when this frame is done.
    const VkDeviceSize scratchSize = std::max(largest, std::min(total, kScratchBudget));
    Buffer scratch = CreateBuffer(
        scratchSize + m_scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    const VkDeviceAddress scratchBase = AlignUp(scratch.address, m_scratchAlignment);

    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers;
    geometries.reserve(m_pending.size());
    size_t next = 0;
    bool first = true;
    while (next < m_pending.size())
    {
        geometries.clear();
        infos.clear();
        ranges.clear();
        rangePointers.clear();
        VkDeviceSize used = 0;
        while (next < m_pending.size())
        {
            RayBlas& blas = *m_pending[next];
            const VkDeviceSize aligned = AlignUp(blas.scratchSize, m_scratchAlignment);
            if (!infos.empty() && used + aligned > scratchSize)
            {
                break;
            }
            geometries.push_back(TriangleGeometry(blas.vertices->address + blas.vertexOffset, blas.triangleCount));
            VkAccelerationStructureBuildGeometryInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.dstAccelerationStructure = blas.handle;
            info.geometryCount = 1;
            info.scratchData.deviceAddress = scratchBase + used;
            infos.push_back(info);
            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = blas.triangleCount;
            ranges.push_back(range);
            used += aligned;
            ++next;
        }
        for (size_t index = 0; index < infos.size(); ++index)
        {
            infos[index].pGeometries = &geometries[index];
            rangePointers.push_back(&ranges[index]);
        }
        if (!first)
        {
            // The last batch's builds are done with the scratch before this one reuses it.
            AccelerationBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        }
        first = false;
        m_functions->cmdBuild(commandBuffer, static_cast<uint32_t>(infos.size()), infos.data(), rangePointers.data());
    }

    Retired& retired = m_retired[frameSlot];
    retired.buffers.push_back(scratch);
    for (const std::shared_ptr<RayBlas>& blas : m_pending)
    {
        // The vertices stay until this frame is done reading them.
        retired.batches.push_back(std::move(blas->vertices));
        blas->vertices.reset();
        blas->built = true;
    }
    LOG_INFO(
        "Ray acceleration: {} bottom levels built, {} triangles, {:.1f} MiB (scratch {:.1f} MiB)",
        m_pending.size(),
        triangles,
        static_cast<double>(storage) / (1024.0 * 1024.0),
        static_cast<double>(scratchSize) / (1024.0 * 1024.0));
    m_pending.clear();
}

VkAccelerationStructureKHR VulkanRayAcceleration::GetTopLevel(uint32_t frameSlot) const
{
    return m_topLevels[frameSlot].handle;
}

size_t VulkanRayAcceleration::GetBottomLevelCount() const
{
    std::lock_guard lock(m_cache->mutex);
    return m_cache->meshes.size();
}

VulkanRayAcceleration::Buffer VulkanRayAcceleration::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) const
{
    Buffer result{};
    result.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = result.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &result.buffer), "Failed to create a ray acceleration buffer");
    try
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, result.buffer, &requirements);
        if (hostVisible)
        {
            // Its own allocation, as it is mapped for as long as it lives.
            VkMemoryAllocateFlagsInfo flagsInfo{};
            flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocateInfo.pNext = &flagsInfo;
            allocateInfo.allocationSize = requirements.size;
            allocateInfo.memoryTypeIndex = FindMemoryType(
                m_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &result.memory), "Failed to allocate a ray acceleration buffer");
            CheckVulkan(vkBindBufferMemory(m_device, result.buffer, result.memory, 0), "Failed to bind a ray acceleration buffer");
            CheckVulkan(vkMapMemory(m_device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped), "Failed to map a ray acceleration buffer");
        }
        else
        {
            requirements.alignment = std::max<VkDeviceSize>(requirements.alignment, kStructureAlignment);
            result.pooled = VulkanMemoryPool::Allocate(
                m_physicalDevice, m_device, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VulkanMemoryPool::Resource::AddressableBuffer);
            CheckVulkan(vkBindBufferMemory(m_device, result.buffer, result.pooled.memory, result.pooled.offset), "Failed to bind a ray acceleration buffer");
        }
        result.address = BufferAddress(m_device, result.buffer);
    }
    catch (...)
    {
        DestroyBuffer(result);
        throw;
    }
    return result;
}

void VulkanRayAcceleration::DestroyBuffer(Buffer& buffer) const
{
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, buffer.memory, nullptr);
    }
    VulkanMemoryPool::Free(m_device, buffer.pooled);
    buffer = Buffer{};
}

void VulkanRayAcceleration::CreateTopLevel(TopLevel& topLevel, size_t capacity) const
{
    topLevel.capacity = capacity;
    topLevel.instances = CreateBuffer(
        sizeof(VkAccelerationStructureInstanceKHR) * capacity,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        true);

    const VkAccelerationStructureGeometryKHR geometry = InstanceGeometry(0);
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    const uint32_t maxInstances = static_cast<uint32_t>(capacity);
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    m_functions->getBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxInstances, &sizes);

    topLevel.storage = CreateBuffer(
        sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        false);
    topLevel.scratch = CreateBuffer(
        sizes.buildScratchSize + m_scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = topLevel.storage.buffer;
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    CheckVulkan(
        m_functions->createAccelerationStructure(m_device, &createInfo, nullptr, &topLevel.handle),
        "Failed to create a top-level acceleration structure");
    topLevel.count = 0;
    topLevel.generation = 0;
    topLevel.dirty = true;
}

void VulkanRayAcceleration::DestroyTopLevel(TopLevel& topLevel) const
{
    if (topLevel.handle != VK_NULL_HANDLE)
    {
        m_functions->destroyAccelerationStructure(m_device, topLevel.handle, nullptr);
    }
    DestroyBuffer(topLevel.storage);
    DestroyBuffer(topLevel.instances);
    DestroyBuffer(topLevel.scratch);
    topLevel = TopLevel{};
}
}
