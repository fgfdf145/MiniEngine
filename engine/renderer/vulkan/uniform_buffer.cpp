#include "uniform_buffer.h"

#include "nvrhi_pass.h"
#include "nvrhi_resources.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace me
{

VulkanUniformBuffer::VulkanUniformBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    uint32_t imageCount,
    nvrhi::IBindingLayout* frameSetLayout,
    TextureDescriptorBinding shadowMap,
    TextureDescriptorBinding localShadowAtlas,
    EnvironmentDescriptorBindings environment,
    uint32_t drawCapacity)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_shadowMap(shadowMap),
      m_localShadowAtlas(localShadowAtlas),
      m_environment(environment),
      m_frameSetLayout(frameSetLayout),
      // A zero-sized storage buffer is invalid, and a scene with no submeshes still binds set 0.
      m_motionSlotCount(std::max(drawCapacity, 1u)),
      m_imageCount(imageCount)
{
    // A content upload builds this object while the previous one is still live, so running out of
    // memory here is a recoverable failure; release whatever was created before rethrowing.
    try
    {
        CreateBuffers(imageCount);
        BuildFrameBindingSets();
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanUniformBuffer::~VulkanUniformBuffer()
{
    DestroyHandles();
}

void VulkanUniformBuffer::DestroyHandles()
{
    // The sets first: they hold the buffers.
    m_frameDescriptorSets.clear();
    m_frameBindingSets.clear();
    // The buffers and their memory (unmapped as it is freed) go with the handles.
    m_lightBuffers.clear();
    m_lightHandles.clear();
    m_mappedLightBuffers.clear();
    m_materialHandle = nullptr;
    m_materialBuffer = VK_NULL_HANDLE;
    m_mappedMaterialBuffer = nullptr;
    m_textureTransformHandle = nullptr;
    m_textureTransformBuffer = VK_NULL_HANDLE;
    m_mappedTextureTransformBuffer = nullptr;
    m_clusterBuffers.clear();
    m_clusterHandles.clear();
    m_mappedClusterBuffers.clear();
    m_shadowTileBuffers.clear();
    m_shadowTileHandles.clear();
    m_mappedShadowTileBuffers.clear();
    m_buffers.clear();
    m_handles.clear();
    m_mappedBuffers.clear();
    m_motionBuffers.clear();
    m_motionHandles.clear();
    m_mappedMotionBuffers.clear();
    // m_frameSetLayout is owned by VulkanFrameDescriptorSetLayout, not by this buffer.
}

void VulkanUniformBuffer::SetEnvironmentMap(TextureDescriptorBinding environmentMap)
{
    m_environment.environmentMap = environmentMap;
    BuildFrameBindingSets();
}

void VulkanUniformBuffer::SetScatterImages(TextureDescriptorBinding light, TextureDescriptorBinding depth)
{
    m_environment.scatterLight = light;
    m_environment.scatterDepth = depth;
    BuildFrameBindingSets();
}

void VulkanUniformBuffer::SetPathTraceLayerImages(TextureDescriptorBinding depth, TextureDescriptorBinding diffuse, TextureDescriptorBinding specular)
{
    m_environment.pathTraceLayerDepth = depth;
    m_environment.pathTraceLayerDiffuse = diffuse;
    m_environment.pathTraceLayerSpecular = specular;
    BuildFrameBindingSets();
}

void VulkanUniformBuffer::SetCloudTarget(TextureDescriptorBinding target)
{
    m_environment.cloudTarget = target;
    BuildFrameBindingSets();
}

VkDescriptorSet VulkanUniformBuffer::GetFrameDescriptorSet(uint32_t imageIndex) const
{
    if (imageIndex >= m_imageCount)
    {
        throw std::runtime_error("Frame descriptor set image index is out of range");
    }

    return m_frameDescriptorSets[imageIndex];
}

nvrhi::IBindingSet* VulkanUniformBuffer::GetFrameBindingSet(uint32_t imageIndex) const
{
    if (imageIndex >= m_imageCount)
    {
        throw std::runtime_error("Frame binding set image index is out of range");
    }

    return m_frameBindingSets[imageIndex];
}

void VulkanUniformBuffer::Update(
    uint32_t imageIndex,
    const ViewportMatrices& matrices,
    const glm::vec3& cameraPosition,
    const glm::vec3& ambientLuminance,
    bool usesFallbackAmbient,
    const std::array<glm::vec3, 3>& ambientGradient,
    const LightUpload& lights,
    const ShadowUniformData& shadow,
    const glm::mat4& prevViewProj,
    std::span<const glm::mat4> prevModels,
    std::span<const uint32_t> prevModelSlots,
    const EnvironmentUniformData& environment,
    const glm::mat4& viewProjNoJitter,
    bool specularAntiAliasing,
    float preExposure,
    const DdgiUniformData& ddgi,
    float textureMipBias,
    bool pathTraceLayer,
    uint32_t pathTraceLayerShift)
{
    // A draw whose slot lies past the buffer would read out of bounds on the GPU, and no
    // robustness feature is enabled to catch it, so a mismatch is refused here instead.
    if (prevModels.size() != prevModelSlots.size())
    {
        throw std::runtime_error("Every previous model matrix needs its draw slot");
    }
    for (const uint32_t slot : prevModelSlots)
    {
        if (slot >= m_motionSlotCount)
        {
            throw std::runtime_error("A draw slot lies past the motion slots");
        }
    }

    CameraUniformData data{};
    data.view = matrices.view;
    data.proj = matrices.renderProjection;
    // renderProjection, not projection: it is the Y-flipped matrix the shaders actually use, and
    // inverting the other one would reconstruct every position mirrored.
    data.invViewProj = glm::inverse(matrices.renderProjection * matrices.view);
    data.cameraWorldPosition = glm::vec4(cameraPosition, 1.0f);
    data.ambientLuminance = glm::vec4(ambientLuminance, usesFallbackAmbient ? 1.0f : 0.0f);
    for (size_t channel = 0; channel < ambientGradient.size(); ++channel)
    {
        data.ambientGradient[channel] = glm::vec4(ambientGradient[channel], 0.0f);
    }

    // The shader indexes the light buffer with these counts and with the grid's indices, so all of
    // them are checked against what the buffers hold rather than trusted.
    const uint32_t lightCount = std::min(static_cast<uint32_t>(lights.lights.size()), kMaxSceneLights);
    const bool clustered = lights.clustered && lights.clusters != nullptr;
    data.lightCounts = glm::uvec4(std::min(lights.directionalCount, lightCount), lightCount, clustered ? 1u : 0u, 0u);
    std::memcpy(m_mappedLightBuffers[imageIndex], lights.lights.data(), sizeof(GpuLightData) * lightCount);
    // Every tile a light points at must be one uploaded here.
    const uint32_t tileCount = static_cast<uint32_t>(lights.shadowTiles.size());
    if (tileCount > kLocalShadowTileCount)
    {
        throw std::runtime_error("More local shadow tiles than the tile buffer holds");
    }
    for (uint32_t index = 0; index < lightCount; ++index)
    {
        const float tileRef = lights.lights[index].areaRightAxis.w;
        if (tileRef < 0.0f || tileRef > static_cast<float>(tileCount))
        {
            throw std::runtime_error("A light points past the uploaded local shadow tiles");
        }
    }
    std::memcpy(m_mappedShadowTileBuffers[imageIndex], lights.shadowTiles.data(), lights.shadowTiles.size_bytes());
    if (clustered)
    {
        const LightClusterGrid& grid = *lights.clusters;
        if (grid.ranges.size() != kLightClusterCount || grid.indices.size() > kLightClusterIndexCapacity)
        {
            throw std::runtime_error("Light cluster grid does not fit the cluster buffer");
        }
        for (uint32_t index : grid.indices)
        {
            if (index >= lightCount)
            {
                throw std::runtime_error("Light cluster grid indexes past the uploaded lights");
            }
        }
        data.lightClusterSlices = glm::vec4(grid.sliceScale, grid.sliceBias, 0.0f, 0.0f);
        auto* clusterBytes = static_cast<std::byte*>(m_mappedClusterBuffers[imageIndex]);
        std::memcpy(clusterBytes, grid.ranges.data(), sizeof(glm::uvec2) * kLightClusterCount);
        std::memcpy(clusterBytes + sizeof(glm::uvec2) * kLightClusterCount, grid.indices.data(), sizeof(uint32_t) * grid.indices.size());
    }
    data.shadow = shadow;
    data.prevViewProj = prevViewProj;
    data.environment = environment;
    data.viewProjNoJitter = viewProjNoJitter;
    data.specularAntiAliasing = glm::vec4(specularAntiAliasing ? 1.0f : 0.0f, kSpecularAAVariance, kSpecularAAThreshold, 0.0f);
    data.exposure = glm::vec4(preExposure, 1.0f / preExposure, 0.0f, 0.0f);
    data.ddgi = ddgi;
    data.textureParams = glm::vec4(textureMipBias, pathTraceLayer ? 1.0f : 0.0f, static_cast<float>(pathTraceLayerShift), 0.0f);

    std::memcpy(m_mappedBuffers[imageIndex], &data, sizeof(data));
    auto* motion = static_cast<glm::mat4*>(m_mappedMotionBuffers[imageIndex]);
    for (size_t index = 0; index < prevModels.size(); ++index)
    {
        motion[prevModelSlots[index]] = prevModels[index];
    }
}

VulkanFrameDescriptorSetLayout::VulkanFrameDescriptorSetLayout(nvrhi::IDevice* device)
{
    nvrhi::BindingLayoutDesc desc;
    // Every stage that reads set 0: the vertex shaders (the camera block, the previous model
    // matrices), the fragment shaders and the compute passes.
    desc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel | nvrhi::ShaderType::Compute;
    desc.registerSpace = 0;
    desc.registerSpaceIsDescriptorSet = true;
    // A slot is its binding: the shaders number set 0 themselves.
    desc.bindingOffsets = ShaderBindingOffsets();
    const auto texture = [&desc](uint32_t binding)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(binding));
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(binding + kFrameSamplerBindingOffset));
    };
    // The shaders' StructuredBuffers: structured views (their element size from the buffer), storage
    // buffers on Vulkan.
    const auto buffer = [&desc](uint32_t binding)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(binding));
    };
    // The camera block; the AO passes reconstruct view-space positions from it.
    desc.bindings.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    // The shadow map, sampled with depth comparison by the material fragment shader, and by the ray
    // traced reflections' hit shading beyond the traced shadows' reach.
    texture(1);
    // Each draw's previous model matrix, read by triangle.vert for motion vectors.
    buffer(2);
    // The atmosphere LUTs (3 transmittance, 4 sky-view, 5 aerial perspective) and the HDRI (6),
    // sampled by the sky, lighting and forward fragment shaders, and by the compute shader that
    // projects the sky onto SH.
    for (uint32_t binding = 3; binding <= 6; ++binding)
    {
        texture(binding);
    }
    // The atmosphere's radiance SH, read by the shading of every surface, the DDGI probe rays'
    // among them (ddgi_trace.comp).
    buffer(7);
    // The prefiltered sky (8) and the DFG table (9), for the specular lobe under a physical sky.
    texture(8);
    texture(9);
    // The scene lights (10) and the cluster grid that indexes them (11), read by ShadeSurface.
    buffer(10);
    buffer(11);
    // Every draw's material, read by the material fragment shaders at their draw slot, and by hardware
    // ray tracing's hit shading at the hit's (ray_hit_common.slang).
    buffer(12);
    // The local shadow atlas (13) and its tiles (14), read by ShadeSurface.
    texture(13);
    buffer(14);
    // The area lights' LTC tables.
    texture(15);
    texture(16);
    // Each draw's texture transforms, read by gbuffer.frag and triangle.frag, and by hit shading.
    buffer(17);
    // The transmission copy, sampled by triangle.frag for transmissive surfaces.
    texture(18);
    // The scatter pre-pass's light and depth (VulkanScatterPass), sampled by triangle.frag for
    // materials that scatter (KHR_materials_volume_scatter).
    texture(19);
    texture(20);
    // The DDGI probes: irradiance (21) and visibility (22) atlases and their states (23), read by
    // ShadeSurface and by the probe rays themselves (their infinite bounce).
    texture(21);
    texture(22);
    buffer(23);
    // The volumetric clouds' large (24) and small (25) billows and plume map (27), read by the sky
    // and the environment capture, their shadow map (26), read wherever the sun is shadowed, and
    // the resolved clouds (28) the sky composites.
    for (uint32_t binding = 24; binding <= 28; ++binding)
    {
        texture(binding);
    }
    // The path traced layer of the forward-shaded surfaces: the nearest one's depth (29), which
    // gbuffer.frag's layer pre-pass and triangle.frag match against, and its traced diffuse (30) and
    // specular (31) light, which triangle.frag takes in place of the ambient terms.
    for (uint32_t binding = 29; binding <= 31; ++binding)
    {
        texture(binding);
    }
    // Every material sampler, which the material shaders pick by index.
    desc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(kMaterialSamplerTableBinding).setSize(kMaterialSamplerCount));

    m_layout = device->createBindingLayout(desc);
    if (!m_layout)
    {
        throw std::runtime_error("Failed to create frame descriptor set layout");
    }
}

VkDescriptorSetLayout VulkanFrameDescriptorSetLayout::GetHandle() const
{
    return ToNative<VkDescriptorSetLayout>(m_layout->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSetLayout));
}

nvrhi::IBindingLayout* VulkanFrameDescriptorSetLayout::Get() const
{
    return m_layout;
}

VulkanMaterialDescriptorSetLayout::VulkanMaterialDescriptorSetLayout(nvrhi::IDevice* device)
{
    nvrhi::BindingLayoutDesc desc;
    // The vertex stage too: a toon outline reads its width and the face mask (toon.vert).
    desc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    desc.registerSpace = 1;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.descriptorSetsPerPool = kMaterialSetsPerPool;
    // The textures alone: their samplers are set 0's (kMaterialSamplerTableBinding).
    for (uint32_t binding = 0; binding < kMaterialTextureBindingCount; ++binding)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(binding));
    }
    m_layout = CreateNvrhiBindingLayout(device, desc, "Failed to create the material binding layout");
}

VkDescriptorSetLayout VulkanMaterialDescriptorSetLayout::GetHandle() const
{
    return ToNative<VkDescriptorSetLayout>(m_layout->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSetLayout));
}

nvrhi::IBindingLayout* VulkanMaterialDescriptorSetLayout::Get() const
{
    return m_layout;
}

void VulkanUniformBuffer::CreateBuffers(uint32_t imageCount)
{
    m_buffers.assign(imageCount, VK_NULL_HANDLE);
    m_handles.assign(imageCount, nullptr);
    m_mappedBuffers.assign(imageCount, nullptr);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        CreateMappedBuffer(
            sizeof(CameraUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_buffers[i], m_handles[i], m_mappedBuffers[i], "Failed to create uniform buffer");
    }

    const VkDeviceSize motionBytes = sizeof(glm::mat4) * m_motionSlotCount;
    m_motionBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_motionHandles.assign(imageCount, nullptr);
    m_mappedMotionBuffers.assign(imageCount, nullptr);

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        CreateMappedBuffer(
            motionBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_motionBuffers[i], m_motionHandles[i], m_mappedMotionBuffers[i], "Failed to create previous model buffer", sizeof(glm::mat4));

        // Identity until the first Update, so nothing ever reads uninitialised memory.
        const std::vector<glm::mat4> identities(m_motionSlotCount, glm::mat4(1.0f));
        std::memcpy(m_mappedMotionBuffers[i], identities.data(), static_cast<size_t>(motionBytes));
    }

    constexpr VkDeviceSize kLightBytes = sizeof(GpuLightData) * kMaxSceneLights;
    constexpr VkDeviceSize kClusterBytes =
        sizeof(glm::uvec2) * kLightClusterCount + sizeof(uint32_t) * kLightClusterIndexCapacity;
    m_lightBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_lightHandles.assign(imageCount, nullptr);
    m_mappedLightBuffers.assign(imageCount, nullptr);
    m_clusterBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_clusterHandles.assign(imageCount, nullptr);
    m_mappedClusterBuffers.assign(imageCount, nullptr);
    constexpr VkDeviceSize kShadowTileBytes = sizeof(GpuLocalShadowTile) * kLocalShadowTileCount;
    m_shadowTileBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_shadowTileHandles.assign(imageCount, nullptr);
    m_mappedShadowTileBuffers.assign(imageCount, nullptr);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        CreateMappedBuffer(kShadowTileBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_shadowTileBuffers[i], m_shadowTileHandles[i], m_mappedShadowTileBuffers[i], "Failed to create the local shadow tile buffer", sizeof(GpuLocalShadowTile));
        std::memset(m_mappedShadowTileBuffers[i], 0, static_cast<size_t>(kShadowTileBytes));
        CreateMappedBuffer(kLightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_lightBuffers[i], m_lightHandles[i], m_mappedLightBuffers[i], "Failed to create light buffer", sizeof(GpuLightData));
        CreateMappedBuffer(kClusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_clusterBuffers[i], m_clusterHandles[i], m_mappedClusterBuffers[i], "Failed to create the light cluster buffer", sizeof(uint32_t));
        // Empty until the first Update: no light, no cluster lists anything.
        std::memset(m_mappedLightBuffers[i], 0, static_cast<size_t>(kLightBytes));
        std::memset(m_mappedClusterBuffers[i], 0, static_cast<size_t>(kClusterBytes));
    }

    // A slot is written (WriteDrawSlot) when a draw takes it, and only drawn slots are read: the rest
    // is left as it is (filling a map's tens of thousands of slots with defaults took tens of
    // milliseconds whenever the buffers grew).
    const VkDeviceSize materialBytes = sizeof(GpuMaterialData) * m_motionSlotCount;
    CreateMappedBuffer(materialBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_materialBuffer, m_materialHandle, m_mappedMaterialBuffer, "Failed to create the material buffer", sizeof(GpuMaterialData));
    const VkDeviceSize transformBytes = sizeof(GpuTextureTransforms) * m_motionSlotCount;
    CreateMappedBuffer(transformBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_textureTransformBuffer, m_textureTransformHandle, m_mappedTextureTransformBuffer, "Failed to create the texture transform buffer", sizeof(glm::vec4));
}

uint32_t VulkanUniformBuffer::GetDrawCapacity() const
{
    return m_motionSlotCount;
}

void VulkanUniformBuffer::WriteDrawSlot(uint32_t slot, const GpuMaterialData& material, const GpuTextureTransforms& transforms)
{
    if (slot >= m_motionSlotCount)
    {
        throw std::runtime_error("A draw slot lies past the draw capacity");
    }
    std::memcpy(static_cast<GpuMaterialData*>(m_mappedMaterialBuffer) + slot, &material, sizeof(material));
    std::memcpy(static_cast<GpuTextureTransforms*>(m_mappedTextureTransformBuffer) + slot, &transforms, sizeof(transforms));
}

void VulkanUniformBuffer::CreateMappedBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    nvrhi::BufferHandle& handle,
    void*& mapped,
    const char* failureMessage,
    uint32_t structStride)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    handle = CreateNvrhiBuffer(
        m_nvrhiDevice, bufferInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, failureMessage, &mapped, structStride);
}

void VulkanUniformBuffer::BuildFrameBindingSets()
{
    std::vector<nvrhi::BindingSetHandle> sets(m_imageCount);
    std::vector<VkDescriptorSet> nativeSets(m_imageCount);
    for (uint32_t i = 0; i < m_imageCount; ++i)
    {
        nvrhi::BindingSetDesc desc;
        const auto texture = [&desc](uint32_t binding, const TextureDescriptorBinding& source)
        {
            if (source.texture == nullptr || source.nvrhiSampler == nullptr)
            {
                throw std::runtime_error("Set 0 binding " + std::to_string(binding) + " has no NVRHI texture or sampler");
            }
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(binding, source.texture));
            desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(binding + kFrameSamplerBindingOffset, source.nvrhiSampler));
        };
        const auto buffer = [&desc](uint32_t binding, nvrhi::IBuffer* source)
        {
            if (source == nullptr)
            {
                throw std::runtime_error("Set 0 binding " + std::to_string(binding) + " has no NVRHI buffer");
            }
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(binding, source));
        };
        // The camera block, written once per image here, into the set for that image: not once per
        // material, which is what made the old single-set layout wasteful and is the whole point of
        // this split.
        desc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(0, m_handles[i]));
        texture(1, m_shadowMap);
        buffer(2, m_motionHandles[i]);
        texture(3, m_environment.transmittance);
        texture(4, m_environment.skyView);
        texture(5, m_environment.aerialPerspective);
        texture(6, m_environment.environmentMap);
        buffer(7, m_environment.irradiance);
        texture(8, m_environment.prefiltered);
        texture(9, m_environment.brdfLut);
        buffer(10, m_lightHandles[i]);
        buffer(11, m_clusterHandles[i]);
        buffer(12, m_materialHandle);
        texture(13, m_localShadowAtlas);
        buffer(14, m_shadowTileHandles[i]);
        texture(15, m_environment.ltcInverseMatrices);
        texture(16, m_environment.ltcAmplitudes);
        buffer(17, m_textureTransformHandle);
        texture(18, m_environment.transmission);
        texture(19, m_environment.scatterLight);
        texture(20, m_environment.scatterDepth);
        texture(21, m_environment.ddgiIrradiance);
        texture(22, m_environment.ddgiVisibility);
        buffer(23, m_environment.ddgiProbeStates);
        texture(24, m_environment.cloudShapeNoise);
        texture(25, m_environment.cloudDetailNoise);
        texture(26, m_environment.cloudShadow);
        texture(27, m_environment.cloudWeather);
        texture(28, m_environment.cloudTarget);
        texture(29, m_environment.pathTraceLayerDepth);
        texture(30, m_environment.pathTraceLayerDiffuse);
        texture(31, m_environment.pathTraceLayerSpecular);
        if (m_environment.materialSamplers.size() != kMaterialSamplerCount)
        {
            throw std::runtime_error("Set 0's material sampler table is incomplete");
        }
        for (uint32_t index = 0; index < kMaterialSamplerCount; ++index)
        {
            desc.bindings.push_back(
                nvrhi::BindingSetItem::Sampler(kMaterialSamplerTableBinding, m_environment.materialSamplers[index]).setArrayElement(index));
        }

        sets[i] = m_nvrhiDevice->createBindingSet(desc, m_frameSetLayout);
        if (!sets[i])
        {
            throw std::runtime_error("Failed to create a frame binding set");
        }
        nativeSets[i] = ToNative<VkDescriptorSet>(sets[i]->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSet));
    }
    m_frameBindingSets = std::move(sets);
    m_frameDescriptorSets = std::move(nativeSets);
}
}
