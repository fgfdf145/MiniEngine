#pragma once

#include "common.h"
#include "descriptor_pool_list.h"
#include "ray_acceleration.h"
#include "uniform_buffer.h"

#include <engine/asset/mesh.h>
#include <engine/core/threading/task_future.h>
#include <engine/renderer/material.h>
#include <engine/renderer/ray_tracing_bvh.h>
#include <engine/renderer/reference_path_tracer.h>

#include <array>
#include <cstdint>
#include <future>
#include <mutex>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

// One render submesh of the content, as the ray scene traces it.
struct RaySceneSubmesh
{
    std::shared_ptr<const MeshData> mesh;
    // Rays pass through Blend surfaces.
    bool blend = false;
    // The submesh's draw slot, which is where its ray material lives.
    uint32_t slot = 0;
};

// What a draw slot's ray material is averaged from: given when the slot gets a submesh.
struct RayMaterialSource
{
    uint32_t slot = 0;
    GpuMaterialData material;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;
    // The material's base colour and emissive textures (the defaults when it has none).
    TextureDescriptorBinding baseColor;
    TextureDescriptorBinding emissive;
};

// The scene as compute shaders trace it (shaders/vulkan/ray_tracing_common.glsl): the meshes'
// hierarchies (ray_tracing_bvh.h), built on a worker thread when content changes; each submesh's ray
// material, averaged from its textures on the GPU; and the instances and top level, rebuilt on the CPU
// every frame from the submeshes' model matrices. Device lifetime. Its descriptor set layout (the ray
// set) is what every tracing pass binds as set 1:
//   0 mesh nodes, 1 mesh triangles, 2 instances, 3 top-level nodes, 4 ray materials,
// all storage buffers, and with hardware ray tracing 5, the frame slot's top-level acceleration
// structure over the same instances (VulkanRayAcceleration), which the shaders' RAY_QUERY variants
// trace instead of walking 0 to 3. Nothing is traceable until the first build installs (IsReady).
class VulkanRayScene
{
  public:
    // hardwareRayTracing: the device supports ray queries (VulkanDevice::SupportsRayQuery).
    VulkanRayScene(VkPhysicalDevice physicalDevice, VkDevice device, VkPipelineCache pipelineCache, uint32_t frameCount, bool hardwareRayTracing);
    ~VulkanRayScene();

    VulkanRayScene(const VulkanRayScene&) = delete;
    VulkanRayScene& operator=(const VulkanRayScene&) = delete;

    // New content: starts building the hierarchies of meshes not built before (on a worker) and
    // queues the averaging of the ray materials whose slot changed hands. Slots run below
    // slotCapacity. The textures must stay alive until the next SetContent; the renderer keeps them for
    // as long as the content is live. The caller has waited for every frame in flight (material
    // descriptors are rewritten and freed).
    // placed are the slots that got a submesh with this content, released those that lost theirs; a
    // slot in neither keeps its averaged material.
    // models is each submesh's model matrix now, which the worker builds the top level over.
    void SetContent(
        std::vector<RaySceneSubmesh> submeshes,
        std::vector<glm::mat4> models,
        uint32_t slotCapacity,
        std::span<const RayMaterialSource> placed,
        std::span<const uint32_t> released);

    // Whether a finished build waits to be installed; the caller then waits for every frame in
    // flight and calls InstallBuild, which replaces the buffers the descriptor sets name.
    bool HasFinishedBuild() const;
    void InstallBuild();

    // This frame's instances: models is parallel to the submeshes of the installed content, skipped
    // flags every instance probe rays leave out (kRayInstanceSkip). Writes the frame slot's
    // instance and top-level buffers.
    void UpdateInstances(uint32_t frameSlot, std::span<const glm::mat4> models, std::span<const uint8_t> skipped);

    // Averages the ray materials when content changed; builds the acceleration structures new content
    // or moved instances need, the frame slot's top level only when hardwareRays says this frame's
    // traces use it; afterwards makes everything visible to compute. Record before any pass that traces.
    void Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, bool hardwareRays);

    bool IsReady() const;
    // The set has binding 5 and the passes may trace with ray queries.
    bool HasHardwareRayTracing() const;
    // A build started by SetContent has not been installed yet.
    bool IsBuilding() const;
    VkDescriptorSetLayout GetSetLayout() const;
    VkDescriptorSet GetSet(uint32_t frameSlot) const;
    // Submeshes of the installed content, in the order SetContent gave them.
    size_t GetSubmeshCount() const;

    // The installed hierarchies with the last UpdateInstances' instances and top level, and the ray
    // materials the GPU averaged, for the CPU reference path tracer. Both read the GPU's host-visible
    // buffers and need it idle.
    RayScene CopyCpuScene() const;
    std::vector<ReferenceMaterial> ReadMaterials() const;

  private:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    // A built hierarchy and the mesh it was built from: an address alone could be a new mesh
    // allocated where a freed one was.
    struct BuiltMesh
    {
        std::weak_ptr<const MeshData> mesh;
        std::shared_ptr<const MeshBvh> bvh;
    };
    using BuiltMeshes = std::unordered_map<const MeshData*, BuiltMesh>;
    // Shared by every build, running or not: a change of content starts its build at once instead of
    // waiting for the last one (a streamed map changes every few seconds, and the wait was the most of a
    // change's cost), and each build still finds the hierarchies any other has made.
    struct BuildCache
    {
        std::mutex mutex;
        BuiltMeshes meshes;
    };

    // The worker's result: every mesh concatenated, and each submesh's mesh index; the hierarchies
    // already in GPU buffers, and the top level built over the instances where the content was when it
    // came, so installing it costs the frame nothing but swapping handles.
    struct Build
    {
        RayScene scene;
        std::vector<uint32_t> submeshMeshes;
        // Per submesh, 1 for a Blend material, which rays pass through.
        std::vector<uint8_t> blend;
        Buffer meshNodes;
        Buffer meshTriangles;
        size_t meshNodeCount = 0;
        size_t meshTriangleCount = 0;
        IncrementalTopLevel topLevel;
        // Each mesh's bottom-level acceleration structure (RayScene::meshes order), with hardware ray
        // tracing only.
        std::vector<std::shared_ptr<RayBlas>> blas;
    };
    // A build that will never be installed (or the content a new build replaces) is released on a
    // background task: freeing its hierarchies, hundreds of megabytes of mapped GPU memory on a map, took
    // tens of milliseconds of the frame. The caller has waited for every frame that may read it.
    void DiscardBuild(Build build);
    // The release tasks, waited for before the device goes; finished ones are dropped as new ones come.
    std::vector<TaskFuture<void>> m_releases;

    Buffer CreateBuffer(VkDeviceSize size) const;
    void DestroyBuffer(Buffer& buffer) const;
    void WriteSets();
    void CreateMaterialPipeline(VkPipelineCache pipelineCache);
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_frameCount = 0;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;

    // Static per build.
    Buffer m_meshNodes;
    Buffer m_meshTriangles;
    // Written by the material averaging, per content.
    Buffer m_materials;
    // Per frame slot, sized for the installed content.
    std::vector<Buffer> m_instances;
    std::vector<Buffer> m_topNodes;

    // The material averaging: one set per occupied draw slot, made when the slot gets a submesh and
    // freed when it loses it, and one dispatch for each slot whose submesh changed.
    struct MaterialSlot
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t pool = 0;
        // What was averaged into the slot.
        RayMaterialSource source;
    };
    VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
    std::unique_ptr<VulkanDescriptorPoolList> m_materialPools;
    std::vector<MaterialSlot> m_materialSlots;
    uint32_t m_materialCapacity = 0;
    std::vector<uint32_t> m_dirtyMaterialSlots;
    VkPipelineLayout m_materialPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_materialPipeline = VK_NULL_HANDLE;

    std::vector<RaySceneSubmesh> m_submeshes;
    // Hierarchies already built, by mesh, kept while any content uses them.
    std::shared_ptr<BuildCache> m_buildCache = std::make_shared<BuildCache>();
    TaskFuture<Build> m_pendingBuild;
    // Builds for content that was replaced before they finished, left to finish on their own (a
    // TaskFuture waits in its destructor); dropped once done.
    std::vector<TaskFuture<Build>> m_staleBuilds;
    void DropFinishedStaleBuilds();
    // The installed build's mesh ranges and each submesh's mesh; the per-frame top level starts from
    // them.
    RayScene m_scene;
    IncrementalTopLevel m_topLevel;
    // Bumped whenever the top level changes; a frame slot's buffers hold the generation it last copied.
    uint64_t m_topLevelGeneration = 1;
    std::vector<uint64_t> m_slotGenerations;
    std::vector<uint32_t> m_submeshMeshes;
    std::vector<uint8_t> m_installedBlend;
    // How many mesh nodes and triangles the installed build's buffers hold.
    size_t m_meshNodeCount = 0;
    size_t m_meshTriangleCount = 0;
    bool m_ready = false;

    // Null without hardware ray tracing.
    std::unique_ptr<VulkanRayAcceleration> m_acceleration;
    // By draw slot, 1 where the ray material stops every ray (Opaque, no transmission).
    std::vector<uint8_t> m_opaqueMaterials;
};
}
