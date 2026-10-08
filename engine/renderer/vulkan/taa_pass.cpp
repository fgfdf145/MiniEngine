#include "taa_pass.h"

#include "dlss.h"
#include "nvrhi_resources.h"

#include <array>

namespace me
{

namespace
{
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match TaaConstants in shaders/vulkan/taa_resolve.comp.
struct TaaPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    // Current over previous pre-exposure (see TaaHistoryScale).
    float historyScale = 1.0f;
    uint32_t flags = 0;
    glm::vec2 unused{0.0f};
};
static_assert(sizeof(TaaPushConstants) == 32, "TaaPushConstants must match taa_resolve.comp");

// Must match the TAA_FLAG_* constants in taa_resolve.comp.
constexpr uint32_t kFlagEnabled = 1u;
constexpr uint32_t kFlagHistoryValid = 2u;

constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
// Ray reconstruction's guides: diffuse albedo, specular albedo, normal and roughness, the specular
// hit distance and the reflections' motion vectors.
constexpr std::array<VkFormat, 5> kGuideFormats = {
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R16_SFLOAT,
    VK_FORMAT_R16G16_SFLOAT};
constexpr size_t kGuideHitDistance = 3;
constexpr size_t kGuideReflectionMotion = 4;

// Must match DlssMotionConstants in shaders/vulkan/dlss_motion_vectors.comp.
struct DlssMotionPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    glm::vec2 jitterPixels{0.0f};
    glm::vec2 unused{0.0f};
};
static_assert(sizeof(DlssMotionPushConstants) == 32, "DlssMotionPushConstants must match dlss_motion_vectors.comp");

VkImageMemoryBarrier ImageBarrier(
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkAccessFlags srcAccess,
    VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    return barrier;
}

void RecordBarriers(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStages,
    VkPipelineStageFlags dstStages,
    std::span<const VkImageMemoryBarrier> barriers)
{
    vkCmdPipelineBarrier(
        commandBuffer,
        srcStages,
        dstStages,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data());
}
}

VulkanTaaPass::VulkanTaaPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    try
    {
        m_nearestSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
        m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 6> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(
            m_device,
            pipelineCache,
            frameSetLayout,
            m_setLayout,
            "taa_resolve.comp.spv",
            sizeof(TaaPushConstants),
            m_pipelineLayout,
            m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 4, 2);
        m_history.Create(m_nvrhiDevice, m_device, targets.GetOutputExtent(), kHistoryFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        static constexpr std::array<VkDescriptorType, 3> kMotionTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_motionSetLayout = CreateComputeSetLayout(m_device, kMotionTypes);
        CreateComputePipeline(
            m_device,
            pipelineCache,
            frameSetLayout,
            m_motionSetLayout,
            "dlss_motion_vectors.comp.spv",
            sizeof(DlssMotionPushConstants),
            m_motionPipelineLayout,
            m_motionPipeline);
        m_motionDescriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount(), 2, 1);
        CreateMotionImage(targets.GetExtent());

        static constexpr std::array<VkDescriptorType, 11> kGuideTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_guideSetLayout = CreateComputeSetLayout(m_device, kGuideTypes);
        CreateComputePipeline(
            m_device,
            pipelineCache,
            frameSetLayout,
            m_guideSetLayout,
            "dlss_rr_guides.comp.spv",
            sizeof(DlssMotionPushConstants),
            m_guidePipelineLayout,
            m_guidePipeline);
        m_guideDescriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount(), 6, 5);
        CreateGuideImages(targets.GetExtent());
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTaaPass::~VulkanTaaPass()
{
    DestroyHandles();
}

ScenePassId VulkanTaaPass::Id() const
{
    return ScenePassId::Taa;
}

RenderPassIo VulkanTaaPass::Io() const
{
    // The velocity target is declared in both orders because the bound set names it; in the
    // forward-only order it was never written, and the pass, passing through, never samples it.
    // The G-buffer's albedo, normals, surface and specular make ray reconstruction's guides, and the
    // path tracer's specular result its hit distance (in the alpha); like the velocity they are
    // declared in both orders, and read only when the guides are made.
    static constexpr std::array<RenderTargetId, 8> kReads = {
        RenderTargetId::SceneHdr,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferSpecular,
        RenderTargetId::SceneReflections};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneTaa};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanTaaPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    m_history.RecordBarrier(commandBuffer, frame.taaHistory.valid);
    if (frame.dlss != nullptr)
    {
        RecordDlss(commandBuffer, targets, frame);
        return;
    }

    TaaPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.historyScale = frame.taaHistoryScale;
    constants.flags = (frame.taaEnabled ? kFlagEnabled : 0u) | (frame.taaHistory.valid ? kFlagHistoryValid : 0u);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.taaHistory.readIndex),
        &constants,
        sizeof(constants),
        frame.extent);
}

void VulkanTaaPass::RecordDlss(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // DLSS's motion vectors. The image is rewritten whole, so last frame's contents are discarded; the
    // barrier orders this frame's stores after last frame's DLSS read them.
    {
        const std::array<VkImageMemoryBarrier, 1> barriers = {
            ImageBarrier(m_motionImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)};
        RecordBarriers(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, barriers);
    }
    DlssMotionPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.jitterPixels = frame.jitterPixels;
    DispatchCompute(
        commandBuffer,
        m_motionPipeline,
        m_motionPipelineLayout,
        frame.frameDescriptorSet,
        m_motionDescriptorSets.at(frame.frameSlot),
        &constants,
        sizeof(constants),
        frame.extent);
    {
        const std::array<VkImageMemoryBarrier, 1> barriers = {ImageBarrier(
            m_motionImage,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT)};
        RecordBarriers(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, barriers);
    }

    // Ray reconstruction's guides, as the motion vectors: rewritten whole, then read by NGX.
    if (frame.dlssRayReconstruction)
    {
        std::array<VkImageMemoryBarrier, kGuideFormats.size()> toGeneral{};
        std::array<VkImageMemoryBarrier, kGuideFormats.size()> toRead{};
        for (size_t index = 0; index < m_guides.size(); ++index)
        {
            toGeneral[index] = ImageBarrier(m_guides[index].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
            toRead[index] = ImageBarrier(
                m_guides[index].image,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        }
        RecordBarriers(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, toGeneral);
        DlssMotionPushConstants guideConstants = constants;
        guideConstants.unused.x = frame.pathTraceHitDistance ? 1.0f : 0.0f;
        DispatchCompute(
            commandBuffer,
            m_guidePipeline,
            m_guidePipelineLayout,
            frame.frameDescriptorSet,
            m_guideDescriptorSets.at(frame.frameSlot),
            &guideConstants,
            sizeof(guideConstants),
            frame.extent);
        RecordBarriers(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, toRead);
    }

    // The layout tracker has SceneHdr and the depth in the read layout and SceneTaa in GENERAL, as
    // NGX wants them; NGX leaves them so.
    const uint32_t slot = frame.frameSlot;
    const uint32_t outputSlot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    DlssEvaluateInputs inputs{};
    inputs.color = {
        targets.GetImage(RenderTargetId::SceneHdr, slot),
        targets.GetSampledView(RenderTargetId::SceneHdr, slot),
        targets.GetFormat(RenderTargetId::SceneHdr),
        VK_IMAGE_ASPECT_COLOR_BIT,
        frame.extent};
    inputs.depth = {
        targets.GetImage(RenderTargetId::SceneDepth, slot),
        targets.GetSampledView(RenderTargetId::SceneDepth, slot),
        targets.GetFormat(RenderTargetId::SceneDepth),
        VK_IMAGE_ASPECT_DEPTH_BIT,
        frame.extent};
    inputs.motionVectors = {m_motionImage, m_motionView, kMotionFormat, VK_IMAGE_ASPECT_COLOR_BIT, frame.extent};
    inputs.output = {
        targets.GetImage(RenderTargetId::SceneTaa, outputSlot),
        targets.GetView(RenderTargetId::SceneTaa, outputSlot),
        targets.GetFormat(RenderTargetId::SceneTaa),
        VK_IMAGE_ASPECT_COLOR_BIT,
        frame.outputExtent};
    inputs.jitterPixels = frame.jitterPixels;
    inputs.reset = frame.dlssReset;
    inputs.frameTimeMs = frame.frameTimeMs;
    if (frame.dlssRayReconstruction)
    {
        inputs.diffuseAlbedo = {m_guides[0].image, m_guides[0].view, m_guides[0].format, VK_IMAGE_ASPECT_COLOR_BIT, frame.extent};
        inputs.specularAlbedo = {m_guides[1].image, m_guides[1].view, m_guides[1].format, VK_IMAGE_ASPECT_COLOR_BIT, frame.extent};
        inputs.normalRoughness = {m_guides[2].image, m_guides[2].view, m_guides[2].format, VK_IMAGE_ASPECT_COLOR_BIT, frame.extent};
        if (frame.pathTraceHitDistance)
        {
            const GuideImage& hitDistance = m_guides[kGuideHitDistance];
            const GuideImage& reflectionMotion = m_guides[kGuideReflectionMotion];
            inputs.specularHitDistance = {hitDistance.image, hitDistance.view, hitDistance.format, VK_IMAGE_ASPECT_COLOR_BIT, frame.extent};
            inputs.reflectionMotionVectors = {
                reflectionMotion.image, reflectionMotion.view, reflectionMotion.format, VK_IMAGE_ASPECT_COLOR_BIT, frame.extent};
        }
        inputs.worldToView = frame.view;
        inputs.viewToClip = frame.projection;
    }
    frame.dlss->Evaluate(commandBuffer, inputs);

    // The result becomes the history the SSR trace takes its colour from next frame, as the TAA
    // resolve's does. NGX's own work may be in any stage, hence the wide first scope.
    const VkImage output = inputs.output.image;
    const VkImage history = m_history.GetImage(1u - frame.taaHistory.readIndex);
    {
        const std::array<VkImageMemoryBarrier, 2> barriers = {
            ImageBarrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            ImageBarrier(history, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)};
        RecordBarriers(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, barriers);
    }
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {frame.outputExtent.width, frame.outputExtent.height, 1};
    vkCmdCopyImage(commandBuffer, output, VK_IMAGE_LAYOUT_GENERAL, history, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    // Before the passes that read SceneTaa (and the layout tracker's transition of it, which waits on
    // the compute stage), and next frame's reads of the history.
    {
        const std::array<VkImageMemoryBarrier, 2> barriers = {
            ImageBarrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT),
            ImageBarrier(history, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
        RecordBarriers(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            barriers);
    }
}

VkImageView VulkanTaaPass::GetHistoryView(uint32_t index) const
{
    return m_history.GetView(index);
}

void VulkanTaaPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the TAA TemporalHistory at the same call sites, so the next frame
    // discards the new images' undefined contents. The history holds the output; the motion
    // vectors are at the render size.
    m_history.Create(m_nvrhiDevice, m_device, targets.GetOutputExtent(), kHistoryFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CreateMotionImage(targets.GetExtent());
    CreateGuideImages(targets.GetExtent());
    CreateDescriptorSets(targets);
}

void VulkanTaaPass::CreateGuideImages(VkExtent2D extent)
{
    DestroyGuideImages();
    for (size_t index = 0; index < m_guides.size(); ++index)
    {
        GuideImage& guide = m_guides[index];
        guide.format = kGuideFormats[index];
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = guide.format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        guide.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, guide.image, "Failed to create a ray reconstruction guide image");
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = guide.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = guide.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &guide.view), "Failed to create a ray reconstruction guide view");
    }
}

void VulkanTaaPass::DestroyGuideImages()
{
    for (GuideImage& guide : m_guides)
    {
        if (guide.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, guide.view, nullptr);
        }
        // The image and its memory go with the texture, released with the rest below.
        guide = GuideImage{};
    }
}

void VulkanTaaPass::CreateMotionImage(VkExtent2D extent)
{
    DestroyMotionImage();
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = kMotionFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    m_motionTexture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, m_motionImage, "Failed to create the DLSS motion vector image");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_motionImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kMotionFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_motionView), "Failed to create the DLSS motion vector view");
}

void VulkanTaaPass::DestroyMotionImage()
{
    if (m_motionView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_motionView, nullptr);
        m_motionView = VK_NULL_HANDLE;
    }
    // The image and its memory go with the texture.
    m_motionTexture = nullptr;
    m_motionImage = VK_NULL_HANDLE;
}

void VulkanTaaPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo currentInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::SceneHdr, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{NativeSampler(m_linearSampler), m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneTaa, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 6> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &currentInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyReadInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    m_motionDescriptorSets = AllocateDescriptorSets(m_device, m_motionDescriptorPool, m_motionSetLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorSet set = m_motionDescriptorSets[slot];
        const VkDescriptorImageInfo depthInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
        const VkDescriptorImageInfo velocityInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
        const VkDescriptorImageInfo motionInfo{VK_NULL_HANDLE, m_motionView, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 3> writes = {
            ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
            ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
            ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &motionInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    m_guideDescriptorSets = AllocateDescriptorSets(m_device, m_guideDescriptorPool, m_guideSetLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorSet set = m_guideDescriptorSets[slot];
        const VkDescriptorImageInfo albedoInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::GBufferAlbedo, slot), kReadLayout};
        const VkDescriptorImageInfo normalInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
        const VkDescriptorImageInfo surfaceInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::GBufferSurface, slot), kReadLayout};
        const VkDescriptorImageInfo specularInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::GBufferSpecular, slot), kReadLayout};
        const VkDescriptorImageInfo depthInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
        const VkDescriptorImageInfo diffuseInfo{VK_NULL_HANDLE, m_guides[0].view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo specularAlbedoInfo{VK_NULL_HANDLE, m_guides[1].view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo normalRoughnessInfo{VK_NULL_HANDLE, m_guides[2].view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo reflectionsInfo{NativeSampler(m_nearestSampler), targets.GetSampledView(RenderTargetId::SceneReflections, slot), kReadLayout};
        const VkDescriptorImageInfo hitDistanceInfo{VK_NULL_HANDLE, m_guides[kGuideHitDistance].view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo reflectionMotionInfo{VK_NULL_HANDLE, m_guides[kGuideReflectionMotion].view, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 11> writes = {
            ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &albedoInfo),
            ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
            ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &surfaceInfo),
            ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &specularInfo),
            ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
            ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &diffuseInfo),
            ImageWrite(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &specularAlbedoInfo),
            ImageWrite(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &normalRoughnessInfo),
            ImageWrite(set, 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &reflectionsInfo),
            ImageWrite(set, 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hitDistanceInfo),
            ImageWrite(set, 10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &reflectionMotionInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanTaaPass::DestroyHandles()
{
    DestroyMotionImage();
    DestroyGuideImages();
    if (m_guidePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_guidePipeline, nullptr);
        m_guidePipeline = VK_NULL_HANDLE;
    }
    if (m_guidePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_guidePipelineLayout, nullptr);
        m_guidePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_guideDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_guideDescriptorPool, nullptr);
        m_guideDescriptorPool = VK_NULL_HANDLE;
    }
    m_guideDescriptorSets.clear();
    if (m_guideSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_guideSetLayout, nullptr);
        m_guideSetLayout = VK_NULL_HANDLE;
    }
    if (m_motionPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_motionPipeline, nullptr);
        m_motionPipeline = VK_NULL_HANDLE;
    }
    if (m_motionPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_motionPipelineLayout, nullptr);
        m_motionPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_motionDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_motionDescriptorPool, nullptr);
        m_motionDescriptorPool = VK_NULL_HANDLE;
    }
    m_motionDescriptorSets.clear();
    if (m_motionSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_motionSetLayout, nullptr);
        m_motionSetLayout = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    m_history.Destroy();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    m_nearestSampler = nullptr;
    m_linearSampler = nullptr;
}
}
