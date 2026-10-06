#pragma once

#include "common.h"
#include "memory_pool.h"

#include <engine/asset/mesh.h>
#include <engine/renderer/ray_tracing_bvh.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

struct RayTracingFunctions;

// One mesh's bottom-level acceleration structure, shared by every content that holds the mesh and
// freed with the last of them. Made on the ray scene's worker, built on the GPU by the first
// VulkanRayAcceleration::Record after a content holding it installs. Its triangles are the mesh
// hierarchy's (MeshBvh::triangles, leaf order), so a hit's primitive index plus the instance's
// triangle offset is the same index the compute walk reports.
struct RayBlas
{
    ~RayBlas();

    std::shared_ptr<const RayTracingFunctions> functions;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VulkanPooledMemory memory;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;

    // The build's input until it is recorded: three vertices per triangle in a batch the worker
    // filled, which goes once every structure reading it has been built.
    struct VertexBatch;
    std::shared_ptr<VertexBatch> vertices;
    VkDeviceSize vertexOffset = 0;
    uint32_t triangleCount = 0;
    VkDeviceSize scratchSize = 0;
    // Render thread only.
    bool built = false;
};

// Hardware ray tracing for the ray scene (docs/design/2026-10-07-hardware-ray-tracing-design.md): a
// bottom level per mesh, cached by mesh across contents, and a top level per frame slot over the ray
// scene's instances in their leaf order, so a hit's instance custom index is its RayInstance index and
// every lookup the shaders make after a hit (material, normal, coverage) reads the same arrays the
// compute walk does. Exists only when the device supports ray queries (VulkanDevice::SupportsRayQuery).
class VulkanRayAcceleration
{
  public:
    VulkanRayAcceleration(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t frameCount);
    ~VulkanRayAcceleration();

    VulkanRayAcceleration(const VulkanRayAcceleration&) = delete;
    VulkanRayAcceleration& operator=(const VulkanRayAcceleration&) = delete;

    // Thread-safe, for the ray scene's worker: each mesh's bottom level, parallel to meshes (null for an
    // empty one). A mesh with a live bottom level gets it; the others get new, unbuilt ones whose
    // triangles go into one vertex batch.
    std::vector<std::shared_ptr<RayBlas>> Prepare(
        std::span<const std::shared_ptr<const MeshData>> meshes,
        std::span<const std::shared_ptr<const MeshBvh>> bvhs) const;

    // Render thread, every frame in flight idle: the installed content's bottom levels, indexed like
    // RayScene::meshes, whose unbuilt ones the next Record builds, and every frame slot's top level
    // sized for instanceCapacity instances. Returns the previous content's bottom levels, for the caller
    // to release off the frame's thread. Top-level handles may change: rewrite the descriptor sets.
    std::vector<std::shared_ptr<RayBlas>> Install(
        std::vector<std::shared_ptr<RayBlas>> meshBlas,
        std::span<const RayMeshRange> meshes,
        size_t instanceCapacity);

    // This frame slot's top level from the scene's instances, when generation differs from the one the
    // slot holds. opaqueMaterials, by ray material slot, is 1 where the material stops every ray (its
    // coverage is 1), which lets the hardware skip the coverage test there. Also frees what the slot's
    // last frame retired, so call it once per frame after the slot's fence.
    void UpdateTopLevel(uint32_t frameSlot, const RayScene& scene, uint64_t generation, std::span<const uint8_t> opaqueMaterials);

    // The pending bottom-level builds, then the slot's top level if its instances changed and
    // buildTopLevel says rays will use it, then a barrier for ray queries in compute shaders.
    void Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, bool buildTopLevel);

    VkAccelerationStructureKHR GetTopLevel(uint32_t frameSlot) const;
    // How many meshes the bottom-level cache holds.
    size_t GetBottomLevelCount() const;

  private:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VulkanPooledMemory pooled;          // device-local buffers
        VkDeviceMemory memory = VK_NULL_HANDLE; // host-visible ones, mapped
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceAddress address = 0;
    };
    struct TopLevel
    {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        Buffer storage;
        Buffer instances;
        Buffer scratch;
        size_t capacity = 0;
        uint32_t count = 0;
        uint64_t generation = 0;
        bool dirty = false;
        // The ray instances the instance buffer was written from: between two frames only what moved
        // differs (IncrementalTopLevel keeps the full build's instances in place), and only that is
        // written again.
        std::vector<RayInstance> written;
    };
    struct BuiltMesh
    {
        std::weak_ptr<const MeshData> mesh;
        std::weak_ptr<RayBlas> blas;
    };
    struct BlasCache
    {
        std::mutex mutex;
        std::unordered_map<const MeshData*, BuiltMesh> meshes;
    };

    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) const;
    void DestroyBuffer(Buffer& buffer) const;
    void CreateTopLevel(TopLevel& topLevel, size_t capacity) const;
    void DestroyTopLevel(TopLevel& topLevel) const;
    void RecordBottomLevels(VkCommandBuffer commandBuffer, uint32_t frameSlot);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_frameCount = 0;
    std::shared_ptr<const RayTracingFunctions> m_functions;
    VkDeviceSize m_scratchAlignment = 1;

    std::shared_ptr<BlasCache> m_cache = std::make_shared<BlasCache>();
    std::vector<std::shared_ptr<RayBlas>> m_meshBlas;
    // Bottom levels of the installed content not built yet.
    std::vector<std::shared_ptr<RayBlas>> m_pending;
    // The nodes' offset of each installed mesh (RayInstance::data.x) -> its bottom level's address.
    std::unordered_map<uint32_t, VkDeviceAddress> m_addressByNodeOffset;
    std::vector<TopLevel> m_topLevels;
    // What a frame slot's commands still read: freed when the slot comes round again.
    struct Retired
    {
        std::vector<Buffer> buffers;
        std::vector<std::shared_ptr<RayBlas::VertexBatch>> batches;
    };
    std::vector<Retired> m_retired;
};
}
