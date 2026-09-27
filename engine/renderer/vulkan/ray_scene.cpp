#include "ray_scene.h"

#include "compute_pass_util.h"

#include <engine/core/log/log.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
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

// Matches RayMaterial in ray_tracing_common.glsl: albedo and coverage, emission and flags.
constexpr VkDeviceSize kRayMaterialBytes = 32;

// Every buffer holds at least one element of the largest kind (an instance), so a descriptor always
// names something and the empty scene's dummy instance fits.
VkDeviceSize AtLeastOne(VkDeviceSize size)
{
    return std::max<VkDeviceSize>(size, sizeof(RayInstance));
}
}

VulkanRayScene::VulkanRayScene(VkPhysicalDevice physicalDevice, VkDevice device, VkPipelineCache pipelineCache, uint32_t frameCount)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_frameCount(frameCount)
{
    try
    {
        constexpr std::array<VkDescriptorType, 5> kTypes = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);

        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(kTypes.size()) * m_frameCount};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = m_frameCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the ray scene descriptor pool");
        m_sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, m_frameCount);

        // Placeholder buffers, so the sets are valid before the first build.
        m_meshNodes = CreateBuffer(AtLeastOne(0));
        m_meshTriangles = CreateBuffer(AtLeastOne(0));
        m_materials = CreateBuffer(AtLeastOne(0));
        for (uint32_t slot = 0; slot < m_frameCount; ++slot)
        {
            m_instances.push_back(CreateBuffer(AtLeastOne(0)));
            m_topNodes.push_back(CreateBuffer(AtLeastOne(0)));
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
    // The worker holds nothing of ours but its result.
    if (m_pendingBuild.valid())
    {
        m_pendingBuild.wait();
    }
    DestroyHandles();
}

void VulkanRayScene::SetContent(std::vector<RaySceneSubmesh> submeshes)
{
    // A build still running is for content that no longer exists; its meshes are worth keeping.
    if (m_pendingBuild.valid())
    {
        Build stale = m_pendingBuild.get();
        m_built.merge(stale.built);
    }
    m_ready = false;
    m_submeshes = std::move(submeshes);

    // The ray materials: one per submesh, averaged on the GPU at the next Record.
    DestroyBuffer(m_materials);
    m_materials = CreateBuffer(AtLeastOne(kRayMaterialBytes * m_submeshes.size()));
    if (m_materialPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_materialPool, nullptr);
        m_materialPool = VK_NULL_HANDLE;
    }
    m_materialSets.clear();
    if (!m_submeshes.empty())
    {
        const uint32_t count = static_cast<uint32_t>(m_submeshes.size());
        const std::array<VkDescriptorPoolSize, 2> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * count},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = count;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_materialPool), "Failed to create the ray material descriptor pool");
        m_materialSets = AllocateDescriptorSets(m_device, m_materialPool, m_materialSetLayout, count);
        for (uint32_t index = 0; index < count; ++index)
        {
            const RaySceneSubmesh& submesh = m_submeshes[index];
            const VkDescriptorImageInfo baseColor{submesh.baseColor.sampler, submesh.baseColor.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo emissive{submesh.emissive.sampler, submesh.emissive.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorBufferInfo output{m_materials.buffer, 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 3> writes{};
            writes[0] = ImageWrite(m_materialSets[index], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &baseColor);
            writes[1] = ImageWrite(m_materialSets[index], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &emissive);
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_materialSets[index];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &output;
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
    m_materialsDirty = !m_submeshes.empty();
    WriteSets();

    // The hierarchies, on a worker: only meshes not built before cost anything. The previous
    // content's hierarchies go in with it and come back out pruned to what this content uses.
    std::vector<std::shared_ptr<const MeshData>> meshes;
    std::vector<uint8_t> blend;
    meshes.reserve(m_submeshes.size());
    for (const RaySceneSubmesh& submesh : m_submeshes)
    {
        meshes.push_back(submesh.mesh);
        blend.push_back(submesh.alphaMode == MaterialAlphaMode::Blend ? 1u : 0u);
    }
    m_pendingBuild = std::async(
        std::launch::async,
        [meshes = std::move(meshes), blend = std::move(blend), previous = std::move(m_built)]() mutable
        {
            const auto start = std::chrono::steady_clock::now();
            Build build;
            build.blend = std::move(blend);
            // First the distinct meshes and which need building, then the builds on a few threads
            // (meshes are independent), then the concatenation in submesh order.
            std::unordered_map<const MeshData*, uint32_t> meshIndex;
            std::vector<std::shared_ptr<const MeshData>> distinct;
            std::vector<std::shared_ptr<const MeshBvh>> bvhs;
            for (const std::shared_ptr<const MeshData>& mesh : meshes)
            {
                const MeshData* key = mesh.get();
                const auto [found, inserted] = meshIndex.emplace(key, static_cast<uint32_t>(distinct.size()));
                build.submeshMeshes.push_back(found->second);
                if (!inserted)
                {
                    continue;
                }
                distinct.push_back(mesh);
                const auto cached = previous.find(key);
                bvhs.push_back(cached != previous.end() && cached->second.mesh.lock() == mesh ? cached->second.bvh : nullptr);
            }

            std::atomic<size_t> next{0};
            std::atomic<size_t> newTriangles{0};
            const auto worker = [&]()
            {
                for (size_t index = next++; index < distinct.size(); index = next++)
                {
                    if (bvhs[index])
                    {
                        continue;
                    }
                    const std::shared_ptr<const MeshData>& mesh = distinct[index];
                    std::vector<glm::vec3> positions;
                    if (mesh)
                    {
                        positions.reserve(mesh->vertices.size());
                        for (const Vertex& vertex : mesh->vertices)
                        {
                            positions.emplace_back(vertex.position[0], vertex.position[1], vertex.position[2]);
                        }
                    }
                    bvhs[index] = std::make_shared<const MeshBvh>(mesh ? BuildMeshBvh(positions, mesh->indices) : MeshBvh{});
                    newTriangles += bvhs[index]->triangles.size();
                }
            };
            // Half the hardware threads, as the texture workers take: the frame loop keeps the rest.
            const unsigned threadCount = std::max(1u, std::thread::hardware_concurrency() / 2);
            std::vector<std::thread> threads;
            for (unsigned thread = 1; thread < threadCount; ++thread)
            {
                threads.emplace_back(worker);
            }
            worker();
            for (std::thread& thread : threads)
            {
                thread.join();
            }

            for (size_t index = 0; index < distinct.size(); ++index)
            {
                AppendMesh(build.scene, *bvhs[index]);
                build.built.emplace(distinct[index].get(), BuiltMesh{distinct[index], bvhs[index]});
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            LOG_INFO(
                "Ray scene: {} meshes, {} triangles ({} newly built) in {:.2f} s",
                build.scene.meshes.size(),
                build.scene.meshTriangles.size(),
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
    m_built = std::move(build.built);
    m_submeshMeshes = std::move(build.submeshMeshes);
    m_installedBlend = std::move(build.blend);
    m_scene = std::move(build.scene);

    DestroyBuffer(m_meshNodes);
    DestroyBuffer(m_meshTriangles);
    m_meshNodes = CreateBuffer(AtLeastOne(sizeof(BvhNode) * m_scene.meshNodes.size()));
    m_meshTriangles = CreateBuffer(AtLeastOne(sizeof(BvhTriangle) * m_scene.meshTriangles.size()));
    if (!m_scene.meshNodes.empty())
    {
        std::memcpy(m_meshNodes.mapped, m_scene.meshNodes.data(), sizeof(BvhNode) * m_scene.meshNodes.size());
        std::memcpy(m_meshTriangles.mapped, m_scene.meshTriangles.data(), sizeof(BvhTriangle) * m_scene.meshTriangles.size());
    }
    // The CPU keeps the ranges only: the top level reads nothing else of the meshes. The GPU's copies
    // stay host-visible, so CopyCpuScene can read them back.
    m_meshNodeCount = m_scene.meshNodes.size();
    m_meshTriangleCount = m_scene.meshTriangles.size();
    m_scene.meshNodes.clear();
    m_scene.meshNodes.shrink_to_fit();
    m_scene.meshTriangles.clear();
    m_scene.meshTriangles.shrink_to_fit();

    const size_t instanceCount = m_submeshMeshes.size();
    for (uint32_t slot = 0; slot < m_frameCount; ++slot)
    {
        DestroyBuffer(m_instances[slot]);
        DestroyBuffer(m_topNodes[slot]);
        m_instances[slot] = CreateBuffer(AtLeastOne(sizeof(RayInstance) * instanceCount));
        m_topNodes[slot] = CreateBuffer(AtLeastOne(sizeof(BvhNode) * 2 * instanceCount));
    }
    WriteSets();
    m_ready = true;
}

void VulkanRayScene::UpdateInstances(uint32_t frameSlot, std::span<const glm::mat4> models, std::span<const uint8_t> skipped)
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
        const bool skip = blend || (index < skipped.size() && skipped[index] != 0);
        inputs.push_back(RayInstanceInput{m_submeshMeshes[index], models[index], index, skip ? kRayInstanceSkip : 0u});
    }
    BuildTopLevel(m_scene, inputs);
    if (m_scene.topNodes.empty())
    {
        // Nothing to trace: a root leaf holding one instance every ray skips.
        m_scene.topNodes.push_back(BvhNode{glm::vec3(0.0f), 0u, glm::vec3(0.0f), 1u});
        RayInstance dummy{};
        dummy.data.w = kRayInstanceSkip;
        m_scene.instances.push_back(dummy);
    }
    std::memcpy(m_topNodes[frameSlot].mapped, m_scene.topNodes.data(), sizeof(BvhNode) * m_scene.topNodes.size());
    std::memcpy(m_instances[frameSlot].mapped, m_scene.instances.data(), sizeof(RayInstance) * m_scene.instances.size());
}

void VulkanRayScene::Record(VkCommandBuffer commandBuffer)
{
    if (m_materialsDirty && m_materialPipeline != VK_NULL_HANDLE)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_materialPipeline);
        for (uint32_t index = 0; index < static_cast<uint32_t>(m_submeshes.size()); ++index)
        {
            const RaySceneSubmesh& submesh = m_submeshes[index];
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
                submesh.doubleSided ? kRayMaterialDoubleSided : 0u,
                transmissionBits);
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_materialPipelineLayout, 0, 1, &m_materialSets[index], 0, nullptr);
            vkCmdPushConstants(commandBuffer, m_materialPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(commandBuffer, 1, 1, 1);
        }
        m_materialsDirty = false;
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
}

bool VulkanRayScene::IsReady() const
{
    return m_ready;
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
    materials.resize(m_submeshMeshes.size());
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

VulkanRayScene::Buffer VulkanRayScene::CreateBuffer(VkDeviceSize size) const
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
        // this Mac's unified memory that is also where the GPU reads fastest.
        allocateInfo.memoryTypeIndex = FindMemoryType(
            m_physicalDevice,
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &result.memory), "Failed to allocate a ray scene buffer");
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
    if (m_materialPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_materialPool, nullptr);
        m_materialPool = VK_NULL_HANDLE;
    }
    if (m_materialSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_materialSetLayout, nullptr);
        m_materialSetLayout = VK_NULL_HANDLE;
    }
    DestroyBuffer(m_meshNodes);
    DestroyBuffer(m_meshTriangles);
    DestroyBuffer(m_materials);
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
