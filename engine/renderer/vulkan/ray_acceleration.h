#pragma once

#include "common.h"
#include "memory_pool.h"

#include <engine/asset/mesh.h>
#include <engine/renderer/ray_tracing_bvh.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

struct RayTracingFunctions;

// Top-level instance masks, matching RAY_MASK_* in shaders/vulkan/ray_tracing_common.glsl: the probes'
// rays trace kRayMaskStatic only, the per-pixel visibility rays both.
inline constexpr uint8_t kRayMaskStatic = 0x1;
inline constexpr uint8_t kRayMaskDynamic = 0x2;

// One mesh's bottom-level acceleration structure, shared by every content that holds the mesh and
// freed with the last of them. Made on the ray scene's worker, built on the GPU by the
// VulkanRayAcceleration::Records after a content holding it installs (a few at a time, see
// kBuildTriangleBudget), then compacted: copied into a structure of the size the build turned out to
// need, which takes the original's place. Its triangles are the mesh hierarchy's (MeshBvh::triangles,
// leaf order), so a hit's primitive index plus the instance's triangle offset is the same index the
// compute walk reports.
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
    // Render thread only: built (a top level may name it from the next frame on), and replaced by its
    // compacted copy; the installed content's node offset for it (RayMeshRange::nodeOffset) and that
    // install's number, which says whether the offset is the installed content's.
    bool built = false;
    bool compacted = false;
    // A skinned mesh's: built over its posed position stream (indexed by leaf order, so a hit's
    // primitive index is still the hierarchy's), allowed to update, never compacted, and refitted every
    // frame the top level is built (VulkanRayAcceleration::Record). It holds its leaf-ordered index
    // buffer and its update scratch.
    bool dynamic = false;
    VkDeviceAddress dynamicPositions = 0;
    uint32_t dynamicVertexCount = 0;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    VkDeviceAddress indexAddress = 0;
    VkBuffer updateScratch = VK_NULL_HANDLE;
    VulkanPooledMemory updateScratchMemory;
    VkDeviceAddress updateScratchAddress = 0;
    uint32_t installedNodeOffset = 0;
    uint64_t installNumber = 0;
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
    // A posed mesh (MeshData::IsPosed: skinned, or a tyre) whose buffer has a position address gets a dynamic bottom
    // level over that buffer (RayBlas::dynamic); positionAddresses is parallel to meshes, 0 for none.
    std::vector<std::shared_ptr<RayBlas>> Prepare(
        std::span<const std::shared_ptr<const MeshData>> meshes,
        std::span<const std::shared_ptr<const MeshBvh>> bvhs,
        std::span<const VkDeviceAddress> positionAddresses) const;

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

    // A share of the pending bottom-level builds (and the queries of their compacted sizes), the
    // compactions UpdateTopLevel decided, then the slot's top level if its instances changed and
    // buildTopLevel says rays will use it, then a barrier for ray queries in compute shaders.
    void Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, bool buildTopLevel);

    // True once after the last bottom level a content was waiting for is built: the scene the rays see
    // is now complete (the DDGI probes look again).
    bool TakeBuildsCompleted();

    VkAccelerationStructureKHR GetTopLevel(uint32_t frameSlot) const;
    // How many meshes the bottom-level cache holds.
    size_t GetBottomLevelCount() const;

  private:
    // Refits every built dynamic bottom level (RayBlas::dynamic) to this frame's posed positions;
    // false when there are none.
    bool RecordDynamicUpdates(VkCommandBuffer commandBuffer);

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
        // The bottom levels' state it was written with (m_bottomEpoch): which are built, and where.
        uint64_t bottomEpoch = 0;
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
    // Returns whether it built any.
    bool RecordBottomLevels(VkCommandBuffer commandBuffer, uint32_t frameSlot);
    // The compacted sizes the slot's last frame queried, now readable: each structure that shrinks gets
    // its compacted copy, recorded by this frame's Record, and takes its place at once.
    void StartCompactions(uint32_t frameSlot);
    void RebuildAddressMap();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_frameCount = 0;
    std::shared_ptr<const RayTracingFunctions> m_functions;
    VkDeviceSize m_scratchAlignment = 1;

    std::shared_ptr<BlasCache> m_cache = std::make_shared<BlasCache>();
    std::vector<std::shared_ptr<RayBlas>> m_meshBlas;
    std::vector<RayMeshRange> m_meshRanges;
    // Bottom levels of the installed content not built yet, built from the front.
    std::vector<std::shared_ptr<RayBlas>> m_pending;
    size_t m_pendingNext = 0;
    bool m_buildsCompleted = false;
    // The nodes' offset of each installed mesh (RayInstance::data.x) -> its bottom level's address, for
    // the built ones only: an instance whose structure is not built yet is inactive in the top level.
    std::unordered_map<uint32_t, VkDeviceAddress> m_addressByNodeOffset;
    // Bumped when a bottom level is built or replaced by its compacted copy: every top level is then
    // written again.
    uint64_t m_bottomEpoch = 1;
    uint64_t m_installNumber = 0;
    std::vector<TopLevel> m_topLevels;
    // What a frame slot's commands still read: freed when the slot comes round again.
    struct OldStructure
    {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VulkanPooledMemory memory;
    };
    struct Retired
    {
        std::vector<Buffer> buffers;
        std::vector<std::shared_ptr<RayBlas::VertexBatch>> batches;
        std::vector<OldStructure> structures;
        std::vector<std::shared_ptr<RayBlas>> compacted;
    };
    std::vector<Retired> m_retired;
    void DestroyOldStructure(OldStructure& structure) const;
    // Per frame slot: the structures its last frame built, whose compacted sizes the pool's queries
    // hold in the same order.
    struct CompactionQueries
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<std::shared_ptr<RayBlas>> structures;
    };
    std::vector<CompactionQueries> m_compactionQueries;
    // Structures whose compacted size is known, compacted kCompactionsPerFrame a frame: a cell's
    // hundreds at once made one frame of 40 ms (a structure, its memory and a copy each).
    static constexpr size_t kCompactionsPerFrame = 64;
    std::deque<std::pair<std::shared_ptr<RayBlas>, VkDeviceSize>> m_compactionBacklog;
    // This frame's compacting copies, recorded by Record: from the original to the compacted structure.
    struct CompactionCopy
    {
        VkAccelerationStructureKHR source = VK_NULL_HANDLE;
        // Held until the frame that copies into it is done.
        std::shared_ptr<RayBlas> destination;
    };
    std::vector<CompactionCopy> m_compactionCopies;
    // What compaction has saved so far, for the log.
    VkDeviceSize m_compactedFrom = 0;
    VkDeviceSize m_compactedTo = 0;
};
}
