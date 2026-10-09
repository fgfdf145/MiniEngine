#pragma once

#include "common.h"
#include "nvrhi_native.h"
#include "ray_acceleration.h"
#if MINIENGINE_WITH_D3D12
#include "d3d12_ray_acceleration.h"
#endif
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

namespace nvrhi::d3d12
{
class IDevice;
}

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
    // Its instance's flags (kRayInstance*): kRayInstanceSkip for a surface every ray passes through (a
    // far level of detail, which would double the near one it stands in for), kRayInstanceBlend for a
    // Blend one only the path tracer's rays meet, and kRayInstanceNoShadow for one that casts no
    // shadow. The frame adds kRayInstanceDynamic.
    uint32_t flags = 0;
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

// An installed submesh whose material emits light: the path tracer's emissive light list holds each
// of its triangles (VulkanPathTraceLights).
struct RayEmissiveSubmesh
{
    uint32_t slot = 0;
    uint32_t triangleCount = 0;
};

// The textures hit shading samples per draw slot, in this order, in the ray texture table
// (RAY_TEXTURE_* in shaders/vulkan/ray_hit_common.slang).
inline constexpr uint32_t kRayTexturesPerSlot = 5;

// The scene as compute shaders trace it (shaders/vulkan/ray_tracing_common.slang): the meshes'
// hierarchies (ray_tracing_bvh.h), built on a worker thread when content changes; each submesh's ray
// material, averaged from its textures on the GPU; and the instances and top level, rebuilt on the CPU
// every frame from the submeshes' model matrices. Device lifetime. Its descriptor set layout (the ray
// set) is what every tracing pass binds as set 1:
//   0 mesh nodes, 1 mesh triangles, 2 instances, 3 top-level nodes, 4 ray materials,
// all storage buffers, and with hardware ray tracing 5, the frame slot's top-level acceleration
// structure over the same instances (VulkanRayAcceleration), which the shaders' RAY_QUERY variants
// trace instead of walking 0 to 3, 6 each mesh's vertex and index buffer addresses and 7 each leaf
// triangle's index in its mesh's index list, which hit shading (ray_hit_common.slang) reads the hit's
// vertices through, and 8 every material sampler (the constructor's samplerTable). Hardware ray
// tracing also brings the texture table (GetTextureSet): every draw slot's kRayTexturesPerSlot
// material textures in one array, which hit shading indexes by the hit's slot. Both are NVRHI's: the
// set a binding set per frame slot, over NVRHI handles of the scene's own buffers and top levels, the
// table a bindless descriptor table at set 3 (the overlay port's bindless-table patch), which the
// engine fills with its own batched writes. Nothing is traceable until the first build installs
// (IsReady).
class VulkanRayScene
{
  public:
    // hardwareRayTracing: the device supports ray queries. defaultTexture: a white texture the texture
    // table names where no material's is; it must outlive the ray scene. samplerTable: every material
    // sampler, in VulkanSamplerCache's order. nearGpuMemory: the device has video memory the CPU writes
    // (resizable BAR), where the small buffers every hit reads go. nvrhiVulkanDevice: NVRHI's Vulkan
    // backend, which names the native top levels for the sets and writes the texture table in batches.
    VulkanRayScene(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        nvrhi::vulkan::IDevice* nvrhiVulkanDevice,
        nvrhi::d3d12::IDevice* nvrhiD3D12Device,
        uint32_t frameCount,
        bool hardwareRayTracing,
        TextureDescriptorBinding defaultTexture,
        std::vector<nvrhi::ISampler*> samplerTable,
        bool updateUnusedWhilePending,
        bool nearGpuMemory);
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
    void Record(nvrhi::ICommandList* commandList, uint32_t frameSlot, bool hardwareRays);

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
    // The texture table, with hardware ray tracing only (null handles without): binding 0 the sampled
    // images, kRayTexturesPerSlot entries per draw slot. Each slot's ray material says which sampler
    // of the ray set's binding 8 each of its textures takes.
    VkDescriptorSetLayout GetTextureSetLayout() const;
    VkDescriptorSet GetTextureSet() const;
    // The same as NVRHI sees them, for the passes' NVRHI pipelines (null without hardware ray tracing
    // for the table, and before the first SetContent).
    nvrhi::IBindingLayout* GetNvrhiSetLayout() const;
    nvrhi::IBindingSet* GetBindingSet(uint32_t frameSlot) const;
    nvrhi::IBindingLayout* GetNvrhiTextureSetLayout() const;
    nvrhi::IDescriptorTable* GetTextureTable() const;
    // Submeshes of the installed content, in the order SetContent gave them.
    size_t GetSubmeshCount() const;
    // The installed submeshes that emit light and that every ray may meet (no Blend, no far level of
    // detail), each draw slot once, and a number that changes whenever they may have: a new install or
    // new materials. Their triangles are the leaf triangles from the instance's triangle offset on.
    const std::vector<RayEmissiveSubmesh>& GetEmissiveSubmeshes() const;
    uint64_t GetEmissiveGeneration() const;
    // How many draw slots the ray materials have room for, and how many instances the last
    // UpdateInstances wrote.
    uint32_t GetSlotCapacity() const;
    uint32_t GetInstanceCount() const;
    // Whether any draw slot's material has a coat, a sheen, a dielectric specular or a normal map of
    // its own: the path tracer's hits need its layered variant (PT_LAYERED).
    bool HasLayeredMaterials() const;

    // The installed hierarchies with the last UpdateInstances' instances and top level, and the ray
    // materials the GPU averaged, for the CPU reference path tracer. Both read the GPU's host-visible
    // buffers and need it idle.
    RayScene CopyCpuScene() const;
    std::vector<ReferenceMaterial> ReadMaterials() const;

  private:
    struct Buffer
    {
        nvrhi::BufferHandle handle;
        void* mapped = nullptr;
        uint64_t size = 0;
        // The element size shaders read it as (StructuredBuffer<T>), and whether CreateBuffer was asked
        // for video memory (it may have fallen back to system memory).
        uint32_t stride = 0;
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
        // Per submesh, RaySceneSubmesh::flags.
        std::vector<uint32_t> flags;
        Buffer meshNodes;
        Buffer meshTriangles;
        size_t meshNodeCount = 0;
        size_t meshTriangleCount = 0;
        IncrementalTopLevel topLevel;
        // Each mesh's bottom-level acceleration structure (RayScene::meshes order), with hardware ray
        // tracing only.
        std::vector<std::shared_ptr<RayBlasHandle>> blas;
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

    // Written by the CPU, mapped, read by shaders as StructuredBuffer<T> of stride bytes. nearGpu puts it
    // in video memory the CPU can write (resizable BAR) when the device has such memory and room in it:
    // for the small buffers every hit reads.
    Buffer CreateBuffer(uint64_t size, uint32_t stride, bool nearGpu = false) const;
    // A build's hierarchy buffers: a spare one of the kind that holds size, else a new one with room to
    // grow. RecycleBuffer gives one back (the GPU done with it) for the next build, as freeing hundreds
    // of megabytes of mapped memory held the driver for ~35 ms, which stalled the frame's thread for as
    // long whenever a streamed map's content changed; small buffers are simply destroyed. Both run on
    // the workers.
    Buffer AcquireBuffer(uint64_t size, uint32_t stride, bool nearGpu);
    void RecycleBuffer(Buffer& buffer);
    static constexpr uint64_t kRecycledBufferBytes = uint64_t{4} << 20;
    // One content's large buffers (nodes, triangles, source triangles) and a little more.
    static constexpr size_t kMaxSpareBuffers = 4;
    std::function<void(std::function<void()>)> m_retire;
    // The texture table's binding has VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT.
    bool m_updateUnusedWhilePending = false;
    bool m_nearGpuMemory = false;
    std::mutex m_spareMutex;
    std::vector<Buffer> m_spareBuffers;
    void DestroyBuffer(Buffer& buffer) const;
    // Every slot's set (the frames in flight finished), or one slot's (its last frame finished).
    void WriteSets();
    void WriteSet(uint32_t slot);
    // Runs release now, or once the frames in flight have finished when there is a retire function.
    void Retire(std::function<void()> release);
    // The material averaging's set: the materials buffer and the sampler table.
    void CreateAverageSet();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::vulkan::IDevice* m_nvrhiVulkanDevice = nullptr;
    uint32_t m_frameCount = 0;

    nvrhi::BindingLayoutHandle m_setLayout;
    // Per frame slot; m_sets holds their VkDescriptorSets for the native passes.
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<VkDescriptorSet> m_sets;

    // Static per build.
    Buffer m_meshNodes;
    Buffer m_meshTriangles;
    Buffer m_meshGeometry;
    Buffer m_sourceTriangles;
    std::vector<std::shared_ptr<const VulkanBuffer>> m_meshBuffers;
    // Written by the material averaging, per content: device local, as the GPU writes it.
    nvrhi::BufferHandle m_materials;
    // Per frame slot, sized for m_instanceCapacity instances (InstanceCapacity in ray_scene.cpp).
    std::vector<Buffer> m_instances;
    std::vector<Buffer> m_topNodes;
    size_t m_instanceCapacity = 0;

    // The material averaging: one dispatch for each slot whose submesh changed, reading the slot's
    // textures from the texture table.
    struct MaterialSlot
    {
        // The slot holds a submesh's material.
        bool held = false;
        // What was averaged into the slot.
        RayMaterialSource source;
        // Its textures' indices in the sampler table, packed as RayMaterial::samplers.
        glm::uvec2 samplers{0u};
    };
    nvrhi::BindingLayoutHandle m_averageLayout;
    nvrhi::BindingSetHandle m_averageSet;
    nvrhi::ComputePipelineHandle m_averagePipeline;
    // A grown materials buffer's contents, copied from the old one by the next Record.
    struct MaterialCopy
    {
        nvrhi::BufferHandle source;
        uint64_t bytes = 0;
    };
    MaterialCopy m_materialCopy;
    std::vector<MaterialSlot> m_materialSlots;
    uint32_t m_materialCapacity = 0;
    std::vector<uint32_t> m_dirtyMaterialSlots;

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
    std::vector<uint32_t> m_installedFlags;
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

    // The texture table: allocated for m_textureCapacity draw slots, reallocated when the slots outgrow
    // it. A slot without a material names the default texture, so an installed content still tracing a
    // slot another content released reads something valid. On Vulkan the engine writes it in batches
    // (NVRHI's writeDescriptorTable is one update a call, hundreds of thousands here).
    struct TextureWrites
    {
        std::vector<VkDescriptorImageInfo> infos;
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<std::pair<uint32_t, nvrhi::ITexture*>> entries;
    };
    void WriteTextureSlot(uint32_t slot, const RayMaterialSource* source, TextureWrites& writes) const;
    void FlushTextureWrites(TextureWrites& writes) const;
    TextureDescriptorBinding m_defaultTexture;
    // RayMaterial::samplers for a slot's textures (WriteTextureSlot's, with the default's sampler where
    // the source has no texture); throws for a sampler not in the table.
    glm::uvec2 SamplerIndices(const RayMaterialSource& source) const;
    std::vector<nvrhi::ISampler*> m_samplerTable;
    std::unordered_map<nvrhi::ISampler*, uint32_t> m_samplerIndices;
    nvrhi::BindingLayoutHandle m_textureSetLayout;
    nvrhi::DescriptorTableHandle m_textureTable;
    VkDescriptorSet m_textureSet = VK_NULL_HANDLE;
    uint32_t m_textureCapacity = 0;
    uint32_t m_textureLimit = 0;

    // GetEmissiveSubmeshes's list, remade from the installed submeshes and the slots' materials.
    void UpdateEmissiveSubmeshes();
    std::vector<RayEmissiveSubmesh> m_emissiveSubmeshes;
    uint64_t m_emissiveGeneration = 0;
    bool m_hasLayeredMaterials = false;

    // Null without hardware ray tracing.
    // Vulkan's or D3D12's acceleration structures (IRayAcceleration).
    std::unique_ptr<IRayAcceleration> m_acceleration;
    // By draw slot, 1 where the ray material stops every ray (Opaque, no transmission).
    std::vector<uint8_t> m_opaqueMaterials;
};
}
