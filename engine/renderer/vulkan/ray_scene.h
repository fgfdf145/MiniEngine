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
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

class VulkanBuffer;

// One render submesh of the content, as the ray scene traces it.
struct RaySceneSubmesh
{
    std::shared_ptr<const MeshData> mesh;
    // The mesh's GPU buffers, device addressable with hardware ray tracing: hit shading reads the hit's
    // vertices through them. Held while any content naming them is installed.
    std::shared_ptr<const VulkanBuffer> buffer;
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
    // Its metallic and roughness maps, which hit shading reads with the two above.
    TextureDescriptorBinding metallic;
    TextureDescriptorBinding roughness;
    // Its normal map, which the path tracer's hit shading reads; none when it has no map of its own
    // (the ray material then says so, kRayMaterialNormalMap).
    TextureDescriptorBinding normal;
};

// The textures hit shading samples per draw slot, in this order, in the ray texture table
// (RAY_TEXTURE_* in shaders/vulkan/ray_hit_common.glsl).
inline constexpr uint32_t kRayTexturesPerSlot = 5;

// The scene as compute shaders trace it (shaders/vulkan/ray_tracing_common.glsl): the meshes'
// hierarchies (ray_tracing_bvh.h), built on a worker thread when content changes; each submesh's ray
// material, averaged from its textures on the GPU; and the instances and top level, rebuilt on the CPU
// every frame from the submeshes' model matrices. Device lifetime. Its descriptor set layout (the ray
// set) is what every tracing pass binds as set 1:
//   0 mesh nodes, 1 mesh triangles, 2 instances, 3 top-level nodes, 4 ray materials,
// all storage buffers, and with hardware ray tracing 5, the frame slot's top-level acceleration
// structure over the same instances (VulkanRayAcceleration), which the shaders' RAY_QUERY variants
// trace instead of walking 0 to 3, 6 each mesh's vertex and index buffer addresses and 7 each leaf
// triangle's index in its mesh's index list, which hit shading (ray_hit_common.glsl) reads the hit's
// vertices through. Hardware ray tracing also brings the texture table (GetTextureSet): every draw
// slot's kRayTexturesPerSlot material textures in one array, which hit shading indexes by the hit's
// slot. Nothing is traceable until the first build installs (IsReady).
class VulkanRayScene
{
  public:
    // hardwareRayTracing: the device supports ray queries (VulkanDevice::SupportsRayQuery), and with
    // them the descriptor indexing the texture table needs. defaultTexture (hardware ray tracing only):
    // a white texture the table names where no material's is; it must outlive the ray scene.
    VulkanRayScene(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        uint32_t frameCount,
        bool hardwareRayTracing,
        TextureDescriptorBinding defaultTexture = {},
        bool updateUnusedWhilePending = false);
    ~VulkanRayScene();

    VulkanRayScene(const VulkanRayScene&) = delete;
    VulkanRayScene& operator=(const VulkanRayScene&) = delete;

    // How what the frames in flight may still use is freed: release runs once they have finished
    // (VulkanRetireQueue). Without it everything is freed at once, and every content change must wait
    // for the frames in flight.
    void SetRetire(std::function<void(std::function<void()>)> retire);
    // Whether SetContent needs the frames in flight finished even when slotCapacity does not grow:
    // without a retire function, or when the texture table cannot take descriptors while in use.
    bool ContentChangeWaitsForFrames() const;

    // New content: starts building the hierarchies of meshes not built before (on a worker) and
    // queues the averaging of the ray materials whose slot changed hands. Slots run below
    // slotCapacity. The textures must stay alive until the next SetContent; the renderer keeps them for
    // as long as the content is live. Placed slots must be ones no frame in flight reads. The caller
    // has waited for every frame in flight when slotCapacity grows (every slot's descriptors are
    // written again) or ContentChangeWaitsForFrames says so; otherwise only placed slots' descriptors
    // are written, and released slots' are left as they are (no ray reaches them any more).
    // placed are the slots that got a submesh with this content, released those that lost theirs; a
    // slot in neither keeps its averaged material.
    // models is each submesh's model matrix now, which the worker builds the top level over.
    void SetContent(
        std::vector<RaySceneSubmesh> submeshes,
        std::vector<glm::mat4> models,
        uint32_t slotCapacity,
        std::span<const RayMaterialSource> placed,
        std::span<const uint32_t> released);

    // Whether a finished build waits to be installed. InstallBuild makes it the content rays trace: each
    // frame slot's set names its buffers from that slot's next UpdateInstances, and the content it
    // replaces is retired (SetRetire), so the frames in flight trace on undisturbed. Only when the
    // instance capacity changes, which replaces the slots' own buffers and top levels, does it call
    // waitForFrames first.
    bool HasFinishedBuild() const;
    void InstallBuild(const std::function<void()>& waitForFrames);

    // This frame's instances: models is parallel to the submeshes of the last SetContent, moving
    // flags the instances probe rays leave out and visibility rays still see (kRayInstanceDynamic).
    // While a newer content builds, the installed one keeps tracing: each of its submeshes follows the
    // newer content's submesh with the same draw slot and mesh, and rays skip the ones it no longer
    // holds (what it added casts nothing until it installs). Writes the frame slot's instance and
    // top-level buffers.
    void UpdateInstances(uint32_t frameSlot, std::span<const glm::mat4> models, std::span<const uint8_t> moving);

    // Averages the ray materials when content changed; builds the acceleration structures new content
    // or moved instances need, the frame slot's top level only when hardwareRays says this frame's
    // traces use it; afterwards makes everything visible to compute. Record before any pass that traces.
    void Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, bool hardwareRays);

    bool IsReady() const;
    // True once after the hardware bottom levels a new content waited for are all built (they are
    // built a few at a time): the probes should look at the scene again.
    bool TakeAccelerationCompleted();
    // The set has binding 5 and the passes may trace with ray queries.
    bool HasHardwareRayTracing() const;
    // A build started by SetContent has not been installed yet.
    bool IsBuilding() const;
    VkDescriptorSetLayout GetSetLayout() const;
    VkDescriptorSet GetSet(uint32_t frameSlot) const;
    // The texture table, with hardware ray tracing only (null handles without): one combined image
    // sampler array, binding 0, kRayTexturesPerSlot entries per draw slot.
    VkDescriptorSetLayout GetTextureSetLayout() const;
    VkDescriptorSet GetTextureSet() const;
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
        // What CreateBuffer was asked for (it may have fallen back to system memory).
        bool nearGpu = false;
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
        // Hardware ray tracing only: each mesh's buffers (RayScene::meshes order), held while the build
        // is installed, their addresses (binding 6) and each leaf triangle's source triangle (binding 7).
        std::vector<std::shared_ptr<const VulkanBuffer>> meshBuffers;
        Buffer meshGeometry;
        Buffer sourceTriangles;
    };
    // A build that will never be installed (or the content a new build replaces) is released on a
    // background task: freeing its hierarchies, hundreds of megabytes of mapped GPU memory on a map, took
    // tens of milliseconds of the frame. The caller has waited for every frame that may read it.
    void DiscardBuild(Build build);
    // The release tasks, waited for before the device goes; finished ones are dropped as new ones come.
    std::vector<TaskFuture<void>> m_releases;

    // Host visible and coherent, mapped. nearGpu puts it in video memory the CPU can write (resizable
    // BAR) when the device has such memory and room in it: for the small buffers every hit reads.
    Buffer CreateBuffer(VkDeviceSize size, bool nearGpu = false) const;
    // A build's hierarchy buffers: a spare one of the kind that holds size, else a new one with room to
    // grow. RecycleBuffer gives one back (the GPU done with it) for the next build, as freeing hundreds
    // of megabytes of mapped memory held the driver for ~35 ms, which stalled the frame's thread for as
    // long whenever a streamed map's content changed; small buffers are simply destroyed. Both run on
    // the workers.
    Buffer AcquireBuffer(VkDeviceSize size, bool nearGpu);
    void RecycleBuffer(Buffer& buffer);
    static constexpr VkDeviceSize kRecycledBufferBytes = VkDeviceSize{4} << 20;
    // One content's large buffers (nodes, triangles, source triangles) and a little more.
    static constexpr size_t kMaxSpareBuffers = 4;
    std::function<void(std::function<void()>)> m_retire;
    // The texture table's binding has VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT.
    bool m_updateUnusedWhilePending = false;
    std::mutex m_spareMutex;
    std::vector<Buffer> m_spareBuffers;
    void DestroyBuffer(Buffer& buffer) const;
    // Every slot's set (the frames in flight finished), or one slot's (its last frame finished).
    void WriteSets();
    void WriteSet(uint32_t slot);
    // Runs release now, or once the frames in flight have finished when there is a retire function.
    void Retire(std::function<void()> release);
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
    Buffer m_meshGeometry;
    Buffer m_sourceTriangles;
    std::vector<std::shared_ptr<const VulkanBuffer>> m_meshBuffers;
    // Written by the material averaging, per content.
    Buffer m_materials;
    // Per frame slot, sized for m_instanceCapacity instances (InstanceCapacity in ray_scene.cpp).
    std::vector<Buffer> m_instances;
    std::vector<Buffer> m_topNodes;
    size_t m_instanceCapacity = 0;

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

    // The last SetContent's submeshes, shared with its build and, once installed, as
    // m_installedSubmeshes: tens of thousands on a map, never copied.
    using SubmeshList = std::shared_ptr<const std::vector<RaySceneSubmesh>>;
    SubmeshList m_submeshes = std::make_shared<const std::vector<RaySceneSubmesh>>();
    // Hierarchies already built, by mesh, kept while any content uses them.
    std::shared_ptr<BuildCache> m_buildCache = std::make_shared<BuildCache>();
    TaskFuture<Build> m_pendingBuild;
    // Set when m_pendingBuild becomes stale, so it stops before making its buffers.
    std::shared_ptr<std::atomic<bool>> m_pendingSuperseded;
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
    // What the sets should name changes with each install; a slot's set is written when its frame comes.
    uint64_t m_setsGeneration = 0;
    std::vector<uint64_t> m_slotSetsGenerations;
    std::vector<uint32_t> m_submeshMeshes;
    std::vector<uint8_t> m_installedBlend;
    // The installed content's submeshes, and for each the index of the same submesh in m_submeshes
    // (kNoSubmesh when the content after it dropped it).
    static constexpr uint32_t kNoSubmesh = ~0u;
    SubmeshList m_installedSubmeshes = m_submeshes;
    std::vector<uint32_t> m_installedToCurrent;
    // Each installed submesh's last model matrix: a dropped one stays where it was, skipped.
    std::vector<glm::mat4> m_installedModels;
    void MapInstalledSubmeshes();
    // How many mesh nodes and triangles the installed build's buffers hold.
    size_t m_meshNodeCount = 0;
    size_t m_meshTriangleCount = 0;
    bool m_ready = false;

    // The texture table (hardware ray tracing only): allocated for m_textureCapacity draw slots,
    // reallocated when the slots outgrow it. A slot without a material names the default texture, so
    // an installed content still tracing a slot another content released reads something valid.
    void WriteTextureSlot(uint32_t slot, const RayMaterialSource* source, std::vector<VkDescriptorImageInfo>& infos, std::vector<VkWriteDescriptorSet>& writes) const;
    TextureDescriptorBinding m_defaultTexture;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_texturePool = VK_NULL_HANDLE;
    VkDescriptorSet m_textureSet = VK_NULL_HANDLE;
    uint32_t m_textureCapacity = 0;
    uint32_t m_textureLimit = 0;

    // Null without hardware ray tracing.
    std::unique_ptr<VulkanRayAcceleration> m_acceleration;
    // By draw slot, 1 where the ray material stops every ray (Opaque, no transmission).
    std::vector<uint8_t> m_opaqueMaterials;
};
}
