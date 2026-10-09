// Hardware ray tracing's acceleration structures on Direct3D 12 (docs/design/2026-10-09-d3d12-backend-design.md):
// VulkanRayAcceleration's design (ray_acceleration.h) in D3D12's terms. Bottom levels live in ranges of
// large buffers (D3D12 places a structure anywhere 256-byte aligned in a buffer in the acceleration
// structure state), so tens of thousands of them cost what they hold rather than 64 KiB each.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <directx/d3d12.h>
#include <wrl/client.h>

#include "d3d12_ray_acceleration.h"

#include <engine/core/log/log.h>
#include <engine/core/threading/task_system.h>
#include <engine/renderer/block_suballocator.h>

#include <nvrhi/d3d12.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace me
{

namespace
{
using Microsoft::WRL::ComPtr;

void Check(HRESULT result, const char* what)
{
    if (FAILED(result))
    {
        char code[16];
        std::snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(what) + " (HRESULT 0x" + code + ")");
    }
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

constexpr uint64_t kStructureAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
constexpr uint64_t kScratchAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
// Each triangle as its three vertices, the hierarchy's v0 and v0 plus each edge.
constexpr uint64_t kTriangleVertexBytes = sizeof(float) * 9;
// The same budgets as the Vulkan builds (ray_acceleration.cpp).
constexpr uint64_t kScratchBudget = uint64_t{256} << 20;
constexpr size_t kBuildTriangleBudget = size_t{1} << 21;
constexpr uint32_t kMaxBuildsPerFrame = 1024;
constexpr double kCompactionMinimumSaving = 0.05;
constexpr size_t kCompactionsPerFrame = 64;
// The structures' arenas: blocks of this size, a structure larger than a quarter of one in a buffer
// of its own.
constexpr uint64_t kArenaBlockBytes = uint64_t{64} << 20;

constexpr D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS kBottomLevelFlags =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
constexpr D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS kDynamicBottomLevelFlags =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<uint64_t>(size, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
          "Failed to create a ray acceleration buffer");
    resource->SetName(name);
    return resource;
}

// A host-visible buffer, mapped for its life: what the CPU writes for the builds (triangles, indices,
// instances), which D3D12 reads in GENERIC_READ.
struct UploadBuffer
{
    ComPtr<ID3D12Resource> resource;
    void* mapped = nullptr;
    uint64_t address = 0;
};

UploadBuffer CreateUploadBuffer(ID3D12Device* device, uint64_t size, const wchar_t* name)
{
    UploadBuffer buffer;
    buffer.resource = CreateBuffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, name);
    Check(buffer.resource->Map(0, nullptr, &buffer.mapped), "Failed to map a ray acceleration buffer");
    buffer.address = buffer.resource->GetGPUVirtualAddress();
    return buffer;
}

// Ranges of large device buffers in the acceleration structure state. Thread-safe.
class StructureArena
{
  public:
    struct Range
    {
        ID3D12Resource* buffer = nullptr;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t address = 0;
        size_t block = ~size_t{0};
    };

    explicit StructureArena(ID3D12Device* device)
        : m_device(device)
    {
    }

    Range Allocate(uint64_t size)
    {
        size = AlignUp(size, kStructureAlignment);
        std::lock_guard lock(m_mutex);
        if (size > kArenaBlockBytes / 4)
        {
            Block& block = AddBlock(size);
            block.ranges.Allocate(size, kStructureAlignment);
            return Range{block.buffer.Get(), 0, size, block.buffer->GetGPUVirtualAddress(), m_blocks.size() - 1};
        }
        for (size_t index = 0; index < m_blocks.size(); ++index)
        {
            Block& block = m_blocks[index];
            if (!block.buffer || block.dedicated)
            {
                continue;
            }
            if (const std::optional<uint64_t> offset = block.ranges.Allocate(size, kStructureAlignment))
            {
                return Range{block.buffer.Get(), *offset, size, block.buffer->GetGPUVirtualAddress() + *offset, index};
            }
        }
        Block& block = AddBlock(kArenaBlockBytes);
        block.dedicated = false;
        const std::optional<uint64_t> offset = block.ranges.Allocate(size, kStructureAlignment);
        return Range{block.buffer.Get(), *offset, size, block.buffer->GetGPUVirtualAddress() + *offset, m_blocks.size() - 1};
    }

    void Free(const Range& range)
    {
        if (range.block == ~size_t{0})
        {
            return;
        }
        std::lock_guard lock(m_mutex);
        Block& block = m_blocks.at(range.block);
        block.ranges.Free(range.offset, range.size);
        // A dedicated buffer goes with its structure; a shared block stays for the next ones.
        if (block.dedicated && block.ranges.Empty())
        {
            block.buffer.Reset();
        }
    }

  private:
    struct Block
    {
        ComPtr<ID3D12Resource> buffer;
        BlockSuballocator ranges;
        bool dedicated = true;

        explicit Block(uint64_t size)
            : ranges(size)
        {
        }
    };

    // Called with the mutex held: reuses a dedicated slot whose buffer went.
    Block& AddBlock(uint64_t size)
    {
        ComPtr<ID3D12Resource> buffer = CreateBuffer(
            m_device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, L"Acceleration structures");
        m_blocks.emplace_back(size);
        m_blocks.back().buffer = std::move(buffer);
        return m_blocks.back();
    }

    ID3D12Device* m_device = nullptr;
    std::mutex m_mutex;
    // Indices stay valid: blocks are only appended (a dedicated one's buffer goes, its slot stays).
    std::deque<Block> m_blocks;
};

struct D3D12RayBlas final : RayBlasHandle
{
    ~D3D12RayBlas() override
    {
        if (arena)
        {
            arena->Free(storage);
        }
    }

    std::shared_ptr<StructureArena> arena;
    StructureArena::Range storage;
    uint64_t address = 0;

    struct VertexBatch
    {
        UploadBuffer buffer;
    };
    std::shared_ptr<VertexBatch> vertices;
    uint64_t vertexOffset = 0;
    uint32_t triangleCount = 0;
    uint64_t scratchSize = 0;
    bool built = false;
    bool compacted = false;
    bool dynamic = false;
    uint64_t dynamicPositions = 0;
    uint32_t dynamicVertexCount = 0;
    UploadBuffer indices;
    ComPtr<ID3D12Resource> updateScratch;
    uint32_t installedNodeOffset = 0;
    uint64_t installNumber = 0;
};

D3D12_RAYTRACING_GEOMETRY_DESC BlasGeometry(const D3D12RayBlas& blas)
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // Not opaque: the instances say whether their material stops every ray; each candidate is tested once.
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles = geometry.Triangles;
    triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
    if (blas.dynamic)
    {
        triangles.VertexBuffer.StartAddress = blas.dynamicPositions;
        triangles.VertexCount = blas.dynamicVertexCount;
        triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        triangles.IndexCount = blas.triangleCount * 3;
        triangles.IndexBuffer = blas.indices.address;
    }
    else
    {
        triangles.VertexBuffer.StartAddress = blas.vertices ? blas.vertices->buffer.address + blas.vertexOffset : 0;
        triangles.VertexCount = blas.triangleCount * 3;
    }
    return geometry;
}

void UavBarrier(ID3D12GraphicsCommandList4* commandList)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &barrier);
}

void TransitionBarrier(ID3D12GraphicsCommandList4* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList->ResourceBarrier(1, &barrier);
}

class D3D12RayAcceleration final : public IRayAcceleration
{
  public:
    D3D12RayAcceleration(nvrhi::d3d12::IDevice* nvrhiDevice, uint32_t frameCount)
        : m_nvrhiDevice(nvrhiDevice), m_frameCount(frameCount)
    {
        auto* device = static_cast<ID3D12Device*>(nvrhiDevice->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer);
        Check(device->QueryInterface(IID_PPV_ARGS(&m_device)), "The D3D12 device has no ray tracing (ID3D12Device5)");
        m_arena = std::make_shared<StructureArena>(m_device.Get());
        m_retired.resize(m_frameCount);
        m_topLevels.resize(m_frameCount);
        m_compaction.resize(m_frameCount);
        for (CompactionSizes& sizes : m_compaction)
        {
            sizes.postbuild = CreateBuffer(
                m_device.Get(), sizeof(uint64_t) * kMaxBuildsPerFrame, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true,
                L"Compacted sizes");
            sizes.readback = CreateBuffer(
                m_device.Get(), sizeof(uint64_t) * kMaxBuildsPerFrame, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false,
                L"Compacted sizes readback");
            void* mapped = nullptr;
            Check(sizes.readback->Map(0, nullptr, &mapped), "Failed to map the compacted sizes");
            sizes.mapped = static_cast<const uint64_t*>(mapped);
        }
        // One instance each until content installs, so the binding sets name a structure.
        for (TopLevel& topLevel : m_topLevels)
        {
            CreateTopLevel(topLevel, 1);
        }
    }

    ~D3D12RayAcceleration() override
    {
        for (CompactionSizes& sizes : m_compaction)
        {
            if (sizes.readback)
            {
                sizes.readback->Unmap(0, nullptr);
            }
        }
        for (TopLevel& topLevel : m_topLevels)
        {
            m_arena->Free(topLevel.storage);
        }
    }

    std::vector<std::shared_ptr<RayBlasHandle>> Prepare(
        std::span<const std::shared_ptr<const MeshData>> meshes,
        std::span<const std::shared_ptr<const MeshBvh>> bvhs,
        std::span<const uint64_t> positionAddresses) const override
    {
        const auto positionAddress = [&](uint32_t index) -> uint64_t
        {
            return index < positionAddresses.size() && meshes[index]->IsPosed() ? positionAddresses[index] : 0;
        };
        std::vector<std::shared_ptr<RayBlasHandle>> result(meshes.size());
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
                    const std::shared_ptr<D3D12RayBlas> blas = cached->second.blas.lock();
                    if (blas && blas->dynamicPositions == positionAddress(index))
                    {
                        result[index] = blas;
                    }
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

        // The new meshes' triangles in one upload buffer however many meshes.
        std::vector<uint64_t> offsets(fresh.size());
        uint64_t total = 0;
        for (size_t index = 0; index < fresh.size(); ++index)
        {
            offsets[index] = total;
            total += kTriangleVertexBytes * bvhs[fresh[index]]->triangles.size();
        }
        auto batch = std::make_shared<D3D12RayBlas::VertexBatch>();
        batch->buffer = CreateUploadBuffer(m_device.Get(), total, L"Ray triangles");

        std::vector<std::shared_ptr<D3D12RayBlas>> made(fresh.size());
        TaskSystem::ParallelFor(
            static_cast<uint32_t>(fresh.size()),
            16,
            [&](uint32_t begin, uint32_t end)
            {
                for (uint32_t index = begin; index < end; ++index)
                {
                    const MeshBvh& bvh = *bvhs[fresh[index]];
                    auto* vertices = reinterpret_cast<float*>(static_cast<std::byte*>(batch->buffer.mapped) + offsets[index]);
                    for (const BvhTriangle& triangle : bvh.triangles)
                    {
                        const glm::vec3 v0(triangle.v0);
                        const glm::vec3 v1 = v0 + glm::vec3(triangle.e1);
                        const glm::vec3 v2 = v0 + glm::vec3(triangle.e2);
                        const float packed[9] = {v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z};
                        std::memcpy(vertices, packed, sizeof(packed));
                        vertices += 9;
                    }

                    auto blas = std::make_shared<D3D12RayBlas>();
                    blas->arena = m_arena;
                    blas->vertices = batch;
                    blas->vertexOffset = offsets[index];
                    blas->triangleCount = static_cast<uint32_t>(bvh.triangles.size());
                    const MeshData& mesh = *meshes[fresh[index]];
                    blas->dynamicPositions = positionAddress(fresh[index]);
                    blas->dynamic = blas->dynamicPositions != 0;
                    if (blas->dynamic)
                    {
                        // Each leaf triangle's three vertex indices, in leaf order.
                        blas->dynamicVertexCount = static_cast<uint32_t>(mesh.vertices.size());
                        blas->indices = CreateUploadBuffer(m_device.Get(), sizeof(uint32_t) * 3 * bvh.sourceTriangles.size(), L"Ray triangle indices");
                        auto* written = static_cast<uint32_t*>(blas->indices.mapped);
                        for (const uint32_t source : bvh.sourceTriangles)
                        {
                            for (uint32_t corner = 0; corner < 3; ++corner)
                            {
                                *written++ = mesh.indices[source * 3 + corner];
                            }
                        }
                    }

                    D3D12_RAYTRACING_GEOMETRY_DESC geometry = BlasGeometry(*blas);
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
                    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                    inputs.Flags = blas->dynamic ? kDynamicBottomLevelFlags : kBottomLevelFlags;
                    inputs.NumDescs = 1;
                    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    inputs.pGeometryDescs = &geometry;
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
                    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
                    blas->scratchSize = sizes.ScratchDataSizeInBytes;
                    if (blas->dynamic)
                    {
                        blas->updateScratch = CreateBuffer(
                            m_device.Get(), sizes.UpdateScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true,
                            L"Ray update scratch");
                    }
                    blas->storage = m_arena->Allocate(sizes.ResultDataMaxSizeInBytes);
                    blas->address = blas->storage.address;
                    made[index] = std::move(blas);
                }
            },
            TaskPriority::Medium);

        std::lock_guard lock(m_cache->mutex);
        for (size_t index = 0; index < fresh.size(); ++index)
        {
            const uint32_t mesh = fresh[index];
            m_cache->meshes[meshes[mesh].get()] = BuiltMesh{meshes[mesh], made[index]};
            result[mesh] = std::move(made[index]);
        }
        std::erase_if(m_cache->meshes, [](const auto& entry)
                      {
                          return entry.second.mesh.expired() || entry.second.blas.expired();
                      });
        return result;
    }

    std::vector<std::shared_ptr<RayBlasHandle>> Install(
        std::vector<std::shared_ptr<RayBlasHandle>> meshBlas,
        std::span<const RayMeshRange> meshes,
        size_t instanceCapacity) override
    {
        std::vector<std::shared_ptr<RayBlasHandle>> previous(m_meshBlas.begin(), m_meshBlas.end());
        m_meshBlas.clear();
        for (std::shared_ptr<RayBlasHandle>& blas : meshBlas)
        {
            m_meshBlas.push_back(std::static_pointer_cast<D3D12RayBlas>(std::move(blas)));
        }
        m_meshBlas.resize(meshes.size());
        m_meshRanges.assign(meshes.begin(), meshes.end());
        ++m_installNumber;
        m_pending.clear();
        m_pendingNext = 0;
        for (size_t index = 0; index < meshes.size(); ++index)
        {
            const std::shared_ptr<D3D12RayBlas>& blas = m_meshBlas[index];
            if (!blas || meshes[index].nodeCount == 0)
            {
                continue;
            }
            blas->installedNodeOffset = meshes[index].nodeOffset;
            blas->installNumber = m_installNumber;
            if (!blas->built)
            {
                m_pending.push_back(blas);
            }
        }
        RebuildAddressMap();
        ++m_bottomEpoch;
        const size_t capacity = std::max<size_t>(instanceCapacity, 1);
        for (TopLevel& topLevel : m_topLevels)
        {
            if (topLevel.capacity != capacity)
            {
                // The frames that read the old one have finished (the caller waited).
                m_arena->Free(topLevel.storage);
                topLevel = TopLevel{};
                CreateTopLevel(topLevel, capacity);
            }
            topLevel.generation = 0;
            topLevel.count = 0;
            topLevel.dirty = true;
            topLevel.written.clear();
        }
        return previous;
    }

    void UpdateTopLevel(uint32_t frameSlot, const RayScene& scene, uint64_t generation, std::span<const uint8_t> opaqueMaterials) override
    {
        Retired& retired = m_retired[frameSlot];
        for (StructureArena::Range& range : retired.ranges)
        {
            m_arena->Free(range);
        }
        retired = Retired{};

        StartCompactions(frameSlot);

        TopLevel& topLevel = m_topLevels[frameSlot];
        if (topLevel.generation == generation && topLevel.bottomEpoch == m_bottomEpoch)
        {
            return;
        }
        if (topLevel.bottomEpoch != m_bottomEpoch)
        {
            topLevel.written.clear();
            topLevel.bottomEpoch = m_bottomEpoch;
        }
        const size_t count = std::min(scene.instances.size(), topLevel.capacity);
        const size_t known = std::min(topLevel.written.size(), count);
        topLevel.written.resize(count);
        auto* instances = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(topLevel.instances.mapped);
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
                    D3D12_RAYTRACING_INSTANCE_DESC instance{};
                    const auto address = m_addressByNodeOffset.find(source.data.x);
                    const bool skipped = (source.data.w & kRayInstanceSkip) != 0u || address == m_addressByNodeOffset.end() ||
                                         index > kMaxCustomIndex;
                    if (skipped)
                    {
                        instances[index] = instance;
                        continue;
                    }
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
                            instance.Transform[row][column] = objectToWorld[column][row];
                        }
                    }
                    instance.InstanceID = index;
                    const bool dynamic = (source.data.w & kRayInstanceDynamic) != 0u;
                    const bool noShadow = (source.data.w & kRayInstanceNoShadow) != 0u;
                    instance.InstanceMask = (source.data.w & kRayInstanceBlend) != 0u ? kRayMaskBlend
                                            : noShadow ? (dynamic ? kRayMaskDynamicNoShadow : kRayMaskStaticNoShadow)
                                                       : (dynamic ? kRayMaskDynamicCaster : kRayMaskStaticCaster);
                    const uint32_t material = source.data.z;
                    const bool opaque = material < opaqueMaterials.size() && opaqueMaterials[material] != 0u;
                    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE |
                                     (opaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE);
                    instance.AccelerationStructure = address->second;
                    instances[index] = instance;
                }
            },
            TaskPriority::High);
        topLevel.count = static_cast<uint32_t>(count);
        topLevel.generation = generation;
        topLevel.dirty = true;
    }

    void Record(nvrhi::ICommandList* nvrhiCommandList, uint32_t frameSlot, bool buildTopLevel) override
    {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(
            nvrhiCommandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
        ComPtr<ID3D12GraphicsCommandList4> list;
        Check(commandList->QueryInterface(IID_PPV_ARGS(&list)), "The command list has no ray tracing (ID3D12GraphicsCommandList4)");

        bool builtBottom = RecordBottomLevels(list.Get(), frameSlot);
        if (buildTopLevel && RecordDynamicUpdates(list.Get()))
        {
            builtBottom = true;
            m_topLevels[frameSlot].dirty = true;
        }
        if (!m_compactionCopies.empty())
        {
            UavBarrier(list.Get());
            for (CompactionCopy& copy : m_compactionCopies)
            {
                list->CopyRaytracingAccelerationStructure(
                    copy.destination, copy.source, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            }
            m_compactionCopies.clear();
            builtBottom = true;
        }

        TopLevel& topLevel = m_topLevels[frameSlot];
        if (buildTopLevel && topLevel.dirty)
        {
            if (builtBottom)
            {
                UavBarrier(list.Get());
            }
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            build.Inputs.NumDescs = topLevel.count;
            build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.InstanceDescs = topLevel.instances.address;
            build.DestAccelerationStructureData = topLevel.storage.address;
            build.ScratchAccelerationStructureData = topLevel.scratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            topLevel.dirty = false;
        }
        else if (!builtBottom)
        {
            return;
        }
        // Ray queries this frame, and later builds that read these structures.
        UavBarrier(list.Get());
    }

    bool TakeBuildsCompleted() override
    {
        const bool completed = m_buildsCompleted;
        m_buildsCompleted = false;
        return completed;
    }

    nvrhi::rt::AccelStructHandle CreateTopLevelHandle(uint32_t frameSlot) const override
    {
        nvrhi::rt::AccelStructDesc desc;
        desc.isTopLevel = true;
        desc.debugName = "Ray scene top level";
        return m_nvrhiDevice->createHandleForNativeAccelStruct(m_topLevels[frameSlot].storage.address, desc);
    }

    size_t GetBottomLevelCount() const override
    {
        std::lock_guard lock(m_cache->mutex);
        return m_cache->meshes.size();
    }

  private:
    struct TopLevel
    {
        StructureArena::Range storage;
        UploadBuffer instances;
        ComPtr<ID3D12Resource> scratch;
        size_t capacity = 0;
        uint32_t count = 0;
        uint64_t generation = 0;
        uint64_t bottomEpoch = 0;
        bool dirty = false;
        std::vector<RayInstance> written;
    };
    struct BuiltMesh
    {
        std::weak_ptr<const MeshData> mesh;
        std::weak_ptr<D3D12RayBlas> blas;
    };
    struct BlasCache
    {
        std::mutex mutex;
        std::unordered_map<const MeshData*, BuiltMesh> meshes;
    };
    struct Retired
    {
        std::vector<ComPtr<ID3D12Resource>> buffers;
        std::vector<std::shared_ptr<D3D12RayBlas::VertexBatch>> batches;
        std::vector<StructureArena::Range> ranges;
        std::vector<std::shared_ptr<D3D12RayBlas>> compacted;
    };
    struct CompactionSizes
    {
        ComPtr<ID3D12Resource> postbuild;
        ComPtr<ID3D12Resource> readback;
        const uint64_t* mapped = nullptr;
        std::vector<std::shared_ptr<D3D12RayBlas>> structures;
    };
    struct CompactionCopy
    {
        uint64_t source = 0;
        uint64_t destination = 0;
    };

    void CreateTopLevel(TopLevel& topLevel, size_t capacity)
    {
        topLevel.capacity = capacity;
        topLevel.instances = CreateUploadBuffer(m_device.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * capacity, L"Ray instances");
        std::memset(topLevel.instances.mapped, 0, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * capacity);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = static_cast<UINT>(capacity);
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
        topLevel.storage = m_arena->Allocate(sizes.ResultDataMaxSizeInBytes);
        topLevel.scratch = CreateBuffer(
            m_device.Get(), sizes.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, L"Top-level scratch");
        topLevel.count = 0;
        topLevel.generation = 0;
        topLevel.dirty = true;
    }

    void RebuildAddressMap()
    {
        m_addressByNodeOffset.clear();
        for (size_t index = 0; index < m_meshRanges.size() && index < m_meshBlas.size(); ++index)
        {
            const std::shared_ptr<D3D12RayBlas>& blas = m_meshBlas[index];
            if (blas && blas->built && m_meshRanges[index].nodeCount > 0)
            {
                m_addressByNodeOffset[m_meshRanges[index].nodeOffset] = blas->address;
            }
        }
    }

    void StartCompactions(uint32_t frameSlot)
    {
        CompactionSizes& sizes = m_compaction[frameSlot];
        if (!sizes.structures.empty())
        {
            // The slot's last frame copied these sizes back, and has finished.
            std::vector<std::shared_ptr<D3D12RayBlas>> structures = std::move(sizes.structures);
            sizes.structures.clear();
            for (size_t index = 0; index < structures.size(); ++index)
            {
                m_compactionBacklog.emplace_back(std::move(structures[index]), sizes.mapped[index]);
            }
        }
        bool replaced = false;
        for (size_t done = 0; done < kCompactionsPerFrame && !m_compactionBacklog.empty(); ++done)
        {
            const std::shared_ptr<D3D12RayBlas> structure = std::move(m_compactionBacklog.front().first);
            const uint64_t compactedSize = m_compactionBacklog.front().second;
            m_compactionBacklog.pop_front();
            if (structure.use_count() == 1)
            {
                continue;
            }
            D3D12RayBlas& blas = *structure;
            blas.compacted = true;
            if (compactedSize == 0 ||
                static_cast<double>(compactedSize) > static_cast<double>(blas.storage.size) * (1.0 - kCompactionMinimumSaving))
            {
                continue;
            }
            const StructureArena::Range compact = m_arena->Allocate(compactedSize);
            m_compactionCopies.push_back(CompactionCopy{blas.address, compact.address});
            m_compactedFrom += blas.storage.size;
            m_compactedTo += compact.size;
            // The original goes once this frame, whose copy reads it, is done.
            m_retired[frameSlot].ranges.push_back(blas.storage);
            blas.storage = compact;
            blas.address = compact.address;
            if (blas.installNumber == m_installNumber)
            {
                m_addressByNodeOffset[blas.installedNodeOffset] = blas.address;
            }
            replaced = true;
        }
        if (replaced)
        {
            ++m_bottomEpoch;
        }
        if (m_pendingNext >= m_pending.size() && !m_compactionCopies.empty() && m_compactionBacklog.empty())
        {
            LOG_INFO(
                "Ray acceleration (D3D12): compacted bottom levels so far {:.1f} -> {:.1f} MiB",
                static_cast<double>(m_compactedFrom) / (1024.0 * 1024.0),
                static_cast<double>(m_compactedTo) / (1024.0 * 1024.0));
        }
    }

    bool RecordDynamicUpdates(ID3D12GraphicsCommandList4* list)
    {
        std::vector<D3D12RayBlas*> dynamic;
        for (const std::shared_ptr<D3D12RayBlas>& blas : m_meshBlas)
        {
            if (blas && blas->dynamic && blas->built)
            {
                dynamic.push_back(blas.get());
            }
        }
        if (dynamic.empty())
        {
            return false;
        }
        // The skinning pass's writes (its buffers back in their read state) and the frames before
        // that read these structures.
        UavBarrier(list);
        for (D3D12RayBlas* blas : dynamic)
        {
            D3D12_RAYTRACING_GEOMETRY_DESC geometry = BlasGeometry(*blas);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            build.Inputs.Flags = kDynamicBottomLevelFlags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            build.Inputs.NumDescs = 1;
            build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.pGeometryDescs = &geometry;
            build.SourceAccelerationStructureData = blas->address;
            build.DestAccelerationStructureData = blas->address;
            build.ScratchAccelerationStructureData = blas->updateScratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        }
        return true;
    }

    bool RecordBottomLevels(ID3D12GraphicsCommandList4* list, uint32_t frameSlot)
    {
        if (m_pendingNext >= m_pending.size())
        {
            return false;
        }
        if (!m_compaction[frameSlot].structures.empty())
        {
            StartCompactions(frameSlot);
        }
        const size_t first = m_pendingNext;
        size_t last = first;
        size_t triangles = 0;
        while (last < m_pending.size() && last - first < kMaxBuildsPerFrame &&
               (last == first || triangles + m_pending[last]->triangleCount <= kBuildTriangleBudget))
        {
            triangles += m_pending[last]->triangleCount;
            ++last;
        }
        const std::span<const std::shared_ptr<D3D12RayBlas>> batch(m_pending.data() + first, last - first);
        m_pendingNext = last;

        uint64_t largest = 0;
        uint64_t total = 0;
        uint64_t storage = 0;
        for (const std::shared_ptr<D3D12RayBlas>& blas : batch)
        {
            const uint64_t aligned = AlignUp(blas->scratchSize, kScratchAlignment);
            largest = std::max(largest, aligned);
            total += aligned;
            storage += blas->storage.size;
        }
        const uint64_t scratchSize = std::max(largest, std::min(total, kScratchBudget));
        ComPtr<ID3D12Resource> scratch = CreateBuffer(
            m_device.Get(), scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, L"Bottom-level scratch");
        const uint64_t scratchBase = scratch->GetGPUVirtualAddress();

        uint64_t used = 0;
        for (const std::shared_ptr<D3D12RayBlas>& blas : batch)
        {
            const uint64_t aligned = AlignUp(blas->scratchSize, kScratchAlignment);
            if (used + aligned > scratchSize)
            {
                // The builds so far are done with the scratch before the next ones reuse it.
                UavBarrier(list);
                used = 0;
            }
            D3D12_RAYTRACING_GEOMETRY_DESC geometry = BlasGeometry(*blas);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            build.Inputs.Flags = blas->dynamic ? kDynamicBottomLevelFlags : kBottomLevelFlags;
            build.Inputs.NumDescs = 1;
            build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.pGeometryDescs = &geometry;
            build.DestAccelerationStructureData = blas->address;
            build.ScratchAccelerationStructureData = scratchBase + used;
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            used += aligned;
        }

        // Their compacted sizes, read when the slot comes round again.
        CompactionSizes& sizes = m_compaction[frameSlot];
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addresses;
        for (const std::shared_ptr<D3D12RayBlas>& blas : batch)
        {
            if (blas->dynamic)
            {
                blas->compacted = true;
                continue;
            }
            addresses.push_back(blas->address);
            sizes.structures.push_back(blas);
        }
        UavBarrier(list);
        if (!addresses.empty())
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{};
            info.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
            info.DestBuffer = sizes.postbuild->GetGPUVirtualAddress();
            list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, static_cast<UINT>(addresses.size()), addresses.data());
            TransitionBarrier(list, sizes.postbuild.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(sizes.readback.Get(), 0, sizes.postbuild.Get(), 0, sizeof(uint64_t) * addresses.size());
            TransitionBarrier(list, sizes.postbuild.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        Retired& retired = m_retired[frameSlot];
        retired.buffers.push_back(std::move(scratch));
        for (const std::shared_ptr<D3D12RayBlas>& blas : batch)
        {
            retired.batches.push_back(std::move(blas->vertices));
            blas->vertices.reset();
            blas->built = true;
            if (blas->installNumber == m_installNumber)
            {
                m_addressByNodeOffset[blas->installedNodeOffset] = blas->address;
            }
        }
        ++m_bottomEpoch;
        LOG_INFO(
            "Ray acceleration (D3D12): {} bottom levels built, {} triangles, {:.1f} MiB (scratch {:.1f} MiB), {} left",
            batch.size(),
            triangles,
            static_cast<double>(storage) / (1024.0 * 1024.0),
            static_cast<double>(scratchSize) / (1024.0 * 1024.0),
            m_pending.size() - m_pendingNext);
        if (m_pendingNext >= m_pending.size())
        {
            m_pending.clear();
            m_pendingNext = 0;
            m_buildsCompleted = true;
        }
        return true;
    }

    nvrhi::d3d12::IDevice* m_nvrhiDevice = nullptr;
    ComPtr<ID3D12Device5> m_device;
    uint32_t m_frameCount = 0;
    std::shared_ptr<StructureArena> m_arena;
    std::shared_ptr<BlasCache> m_cache = std::make_shared<BlasCache>();
    std::vector<std::shared_ptr<D3D12RayBlas>> m_meshBlas;
    std::vector<RayMeshRange> m_meshRanges;
    std::vector<std::shared_ptr<D3D12RayBlas>> m_pending;
    size_t m_pendingNext = 0;
    bool m_buildsCompleted = false;
    std::unordered_map<uint32_t, uint64_t> m_addressByNodeOffset;
    uint64_t m_bottomEpoch = 1;
    uint64_t m_installNumber = 0;
    std::vector<TopLevel> m_topLevels;
    std::vector<Retired> m_retired;
    std::vector<CompactionSizes> m_compaction;
    std::deque<std::pair<std::shared_ptr<D3D12RayBlas>, uint64_t>> m_compactionBacklog;
    std::vector<CompactionCopy> m_compactionCopies;
    uint64_t m_compactedFrom = 0;
    uint64_t m_compactedTo = 0;
};
}

std::unique_ptr<IRayAcceleration> CreateD3D12RayAcceleration(nvrhi::d3d12::IDevice* device, uint32_t frameCount)
{
    return std::make_unique<D3D12RayAcceleration>(device, frameCount);
}
}
