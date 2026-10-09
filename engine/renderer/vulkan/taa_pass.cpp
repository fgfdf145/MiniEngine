#include "taa_pass.h"

#include "dlss.h"
#include "nvrhi_pass.h"

#include <array>
#include <stdexcept>
#include <string>

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

// The resolve's set: SceneHdr, depth and velocity read with Load (no samplers), the history sampled
// bilinearly (its sampler at 3 + 64), the other history and SceneTaa stored.
constexpr uint32_t kHistorySamplerBinding = 3 + 64;

uint32_t Groups(uint32_t size)
{
    return (size + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize;
}

// Must match DlssMotionConstants in shaders/vulkan/dlss_motion_vectors.comp.
struct DlssMotionPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    glm::vec2 jitterPixels{0.0f};
    glm::vec2 unused{0.0f};
};
static_assert(sizeof(DlssMotionPushConstants) == 32, "DlssMotionPushConstants must match dlss_motion_vectors.comp");

}

VulkanTaaPass::VulkanTaaPass(nvrhi::IDevice* nvrhiDevice, VkDevice device, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_nvrhiDevice(nvrhiDevice),
      m_device(device)
{
    m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    const auto layout = [&](std::initializer_list<nvrhi::BindingLayoutItem> items, const char* failure)
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.registerSpace = 1;
        desc.registerSpaceIsDescriptorSet = true;
        desc.bindingOffsets = ShaderBindingOffsets();
        desc.bindings = items;
        return CreateNvrhiBindingLayout(m_nvrhiDevice, desc, failure);
    };
    using Item = nvrhi::BindingLayoutItem;
    m_setLayout = layout(
        {Item::Texture_SRV(0),
         Item::Texture_SRV(1),
         Item::Texture_SRV(2),
         Item::Texture_SRV(3),
         Item::Sampler(kHistorySamplerBinding),
         Item::Texture_UAV(4),
         Item::Texture_UAV(5),
         Item::PushConstants(0, sizeof(TaaPushConstants))},
        "Failed to create the TAA binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "taa_resolve.comp.spv", {frameSetLayout, m_setLayout});
    m_history.Create(m_nvrhiDevice, m_device, targets.GetOutputExtent(), kHistoryFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    m_motionSetLayout = layout(
        {Item::Texture_SRV(0), Item::Texture_SRV(1), Item::Texture_UAV(2), Item::PushConstants(0, sizeof(DlssMotionPushConstants))},
        "Failed to create the DLSS motion vector binding layout");
    m_motionPipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "dlss_motion_vectors.comp.spv", {frameSetLayout, m_motionSetLayout});
    m_motion = CreateDlssInputImage(targets.GetExtent(), kMotionFormat, "DLSS motion vectors");

    m_guideSetLayout = layout(
        {Item::Texture_SRV(0),
         Item::Texture_SRV(1),
         Item::Texture_SRV(2),
         Item::Texture_SRV(3),
         Item::Texture_SRV(4),
         Item::Texture_UAV(5),
         Item::Texture_UAV(6),
         Item::Texture_UAV(7),
         Item::Texture_SRV(8),
         Item::Texture_UAV(9),
         Item::Texture_UAV(10),
         Item::PushConstants(0, sizeof(DlssMotionPushConstants))},
        "Failed to create the ray reconstruction guide binding layout");
    m_guidePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "dlss_rr_guides.comp.spv", {frameSetLayout, m_guideSetLayout});
    for (size_t index = 0; index < m_guides.size(); ++index)
    {
        m_guides[index] = CreateDlssInputImage(targets.GetExtent(), kGuideFormats[index], "Ray reconstruction guide");
    }
    CreateBindingSets(targets);
}

VulkanTaaPass::~VulkanTaaPass()
{
    m_history.Destroy();
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

    // SceneHdr, depth and velocity are where the native passes left them, in the read layout; the
    // history images (just put in GENERAL) and SceneTaa go back to GENERAL.
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    nvrhi::ITexture* historyRead = m_history.GetTexture(frame.taaHistory.readIndex);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{historyRead, nvrhi::ResourceStates::UnorderedAccess},
         {m_history.GetTexture(1u - frame.taaHistory.readIndex), nvrhi::ResourceStates::UnorderedAccess},
         {targets.GetTexture(RenderTargetId::SceneTaa, slot), nvrhi::ResourceStates::UnorderedAccess}});
    commandList->setTextureState(historyRead, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = m_pipeline;
    state.bindings = {frame.frameBindingSet, m_bindingSets.at(slot * 2 + frame.taaHistory.readIndex)};
    commandList->setComputeState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(Groups(frame.extent.width), Groups(frame.extent.height));
}

void VulkanTaaPass::RecordDlss(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    DlssMotionPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.jitterPixels = frame.jitterPixels;
    {
        // DLSS's motion vectors and ray reconstruction's guides: rewritten whole, then left in
        // ShaderResource for NGX. The states order this frame's stores after last frame's NGX reads.
        nvrhi::ICommandList* commandList = frame.commandList;
        const NvrhiPassScope scope(commandList, {});
        commandList->setTextureState(m_motion.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_motionPipeline;
        state.bindings = {frame.frameBindingSet, m_motionBindingSets.at(frame.frameSlot)};
        commandList->setComputeState(state);
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(Groups(frame.extent.width), Groups(frame.extent.height));
        commandList->setTextureState(m_motion.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

        if (frame.dlssRayReconstruction)
        {
            for (const DlssInputImage& guide : m_guides)
            {
                commandList->setTextureState(guide.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            }
            commandList->commitBarriers();
            DlssMotionPushConstants guideConstants = constants;
            guideConstants.unused.x = frame.pathTraceHitDistance ? 1.0f : 0.0f;
            state.pipeline = m_guidePipeline;
            state.bindings = {frame.frameBindingSet, m_guideBindingSets.at(frame.frameSlot)};
            commandList->setComputeState(state);
            commandList->setPushConstants(&guideConstants, sizeof(guideConstants));
            commandList->dispatch(Groups(frame.extent.width), Groups(frame.extent.height));
            for (const DlssInputImage& guide : m_guides)
            {
                commandList->setTextureState(guide.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            }
        }
    }

    // The layout tracker has SceneHdr and the depth in the read layout and SceneTaa in GENERAL, as
    // NGX wants them; NGX leaves them so.
    const uint32_t slot = frame.frameSlot;
    const uint32_t outputSlot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    const auto guideImage = [](const DlssInputImage& image, VkExtent2D extent)
    {
        return DlssImage{image.image, image.view, image.format, VK_IMAGE_ASPECT_COLOR_BIT, extent, image.texture.Get()};
    };
    DlssEvaluateInputs inputs{};
    inputs.color = {
        targets.GetImage(RenderTargetId::SceneHdr, slot),
        targets.GetSampledView(RenderTargetId::SceneHdr, slot),
        targets.GetFormat(RenderTargetId::SceneHdr),
        VK_IMAGE_ASPECT_COLOR_BIT,
        frame.extent,
        targets.GetTexture(RenderTargetId::SceneHdr, slot)};
    inputs.depth = {
        targets.GetImage(RenderTargetId::SceneDepth, slot),
        targets.GetSampledView(RenderTargetId::SceneDepth, slot),
        targets.GetFormat(RenderTargetId::SceneDepth),
        VK_IMAGE_ASPECT_DEPTH_BIT,
        frame.extent,
        targets.GetTexture(RenderTargetId::SceneDepth, slot)};
    inputs.motionVectors = guideImage(m_motion, frame.extent);
    inputs.output = {
        targets.GetImage(RenderTargetId::SceneTaa, outputSlot),
        targets.GetView(RenderTargetId::SceneTaa, outputSlot),
        targets.GetFormat(RenderTargetId::SceneTaa),
        VK_IMAGE_ASPECT_COLOR_BIT,
        frame.outputExtent,
        targets.GetTexture(RenderTargetId::SceneTaa, outputSlot)};
    inputs.jitterPixels = frame.jitterPixels;
    inputs.reset = frame.dlssReset;
    inputs.frameTimeMs = frame.frameTimeMs;
    if (frame.dlssRayReconstruction)
    {
        inputs.diffuseAlbedo = guideImage(m_guides[0], frame.extent);
        inputs.specularAlbedo = guideImage(m_guides[1], frame.extent);
        inputs.normalRoughness = guideImage(m_guides[2], frame.extent);
        if (frame.pathTraceHitDistance)
        {
            inputs.specularHitDistance = guideImage(m_guides[kGuideHitDistance], frame.extent);
            inputs.reflectionMotionVectors = guideImage(m_guides[kGuideReflectionMotion], frame.extent);
        }
        inputs.worldToView = frame.view;
        inputs.viewToClip = frame.projection;
    }
    // NGX reads its inputs as shader resources and writes the output for unordered access: the
    // transitions before this pass and the guides' states above put them there. It records into the
    // command list's native command buffer and binds what it likes, which NVRHI then forgets.
    nvrhi::ICommandList* commandList = frame.commandList;
    commandList->setTextureState(inputs.color.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(inputs.depth.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(targets.GetTexture(RenderTargetId::SceneTaa, outputSlot), nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
    frame.dlss->Evaluate(commandList, inputs, frame.dlssSlot);
    commandList->clearState();

    // The result becomes the history the SSR trace takes its colour from next frame, as the TAA
    // resolve's does.
    nvrhi::ITexture* output = targets.GetTexture(RenderTargetId::SceneTaa, outputSlot);
    nvrhi::ITexture* history = m_history.GetTexture(1u - frame.taaHistory.readIndex);
    commandList->setTextureState(output, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->setTextureState(history, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    const nvrhi::TextureSlice slice = nvrhi::TextureSlice().setWidth(frame.outputExtent.width).setHeight(frame.outputExtent.height);
    commandList->copyTexture(history, slice, output, slice);
    // Where the TAA resolve leaves the two: SceneTaa written, the history for next frame's reads.
    commandList->setTextureState(output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(history, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
}

VkImageView VulkanTaaPass::GetHistoryView(uint32_t index) const
{
    return m_history.GetView(index);
}

nvrhi::ITexture* VulkanTaaPass::GetHistoryTexture(uint32_t index) const
{
    return m_history.GetTexture(index);
}

void VulkanTaaPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the TAA TemporalHistory at the same call sites, so the next frame
    // discards the new images' undefined contents. The history holds the output; the motion
    // vectors are at the render size.
    m_bindingSets.clear();
    m_motionBindingSets.clear();
    m_guideBindingSets.clear();
    m_history.Create(m_nvrhiDevice, m_device, targets.GetOutputExtent(), kHistoryFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    m_motion = CreateDlssInputImage(targets.GetExtent(), kMotionFormat, "DLSS motion vectors");
    for (size_t index = 0; index < m_guides.size(); ++index)
    {
        m_guides[index] = CreateDlssInputImage(targets.GetExtent(), kGuideFormats[index], "Ray reconstruction guide");
    }
    CreateBindingSets(targets);
}

VulkanTaaPass::DlssInputImage VulkanTaaPass::CreateDlssInputImage(VkExtent2D extent, VkFormat format, const char* name) const
{
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.width = extent.width;
    desc.height = extent.height;
    desc.format = ToNvrhiFormat(format);
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    DlssInputImage result;
    result.texture = m_nvrhiDevice->createTexture(desc);
    if (!result.texture)
    {
        throw std::runtime_error(std::string("Failed to create the image for ") + name);
    }
    result.image = ToNative<VkImage>(result.texture->getNativeObject(nvrhi::ObjectTypes::VK_Image));
    result.view = ToNative<VkImageView>(result.texture->getNativeView(nvrhi::ObjectTypes::VK_ImageView));
    result.format = format;
    return result;
}

void VulkanTaaPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    using Item = nvrhi::BindingSetItem;
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_bindingSets.clear();
    m_motionBindingSets.clear();
    m_guideBindingSets.clear();
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const auto target = [&](RenderTargetId id)
        {
            return targets.GetTexture(id, slot);
        };
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                Item::Texture_SRV(0, target(RenderTargetId::SceneHdr)),
                Item::Texture_SRV(1, target(RenderTargetId::SceneDepth)),
                Item::Texture_SRV(2, target(RenderTargetId::GBufferVelocity)),
                Item::Texture_SRV(3, m_history.GetTexture(readIndex)),
                Item::Sampler(kHistorySamplerBinding, m_linearSampler),
                Item::Texture_UAV(4, m_history.GetTexture(1u - readIndex)),
                Item::Texture_UAV(5, target(RenderTargetId::SceneTaa)),
                Item::PushConstants(0, sizeof(TaaPushConstants))};
            m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a TAA binding set"));
        }

        nvrhi::BindingSetDesc motionDesc;
        motionDesc.bindings = {
            Item::Texture_SRV(0, target(RenderTargetId::SceneDepth)),
            Item::Texture_SRV(1, target(RenderTargetId::GBufferVelocity)),
            Item::Texture_UAV(2, m_motion.texture),
            Item::PushConstants(0, sizeof(DlssMotionPushConstants))};
        m_motionBindingSets.push_back(
            CreateNvrhiBindingSet(m_nvrhiDevice, motionDesc, m_motionSetLayout, "Failed to create a DLSS motion vector binding set"));

        nvrhi::BindingSetDesc guideDesc;
        guideDesc.bindings = {
            Item::Texture_SRV(0, target(RenderTargetId::GBufferAlbedo)),
            Item::Texture_SRV(1, target(RenderTargetId::GBufferNormal)),
            Item::Texture_SRV(2, target(RenderTargetId::GBufferSurface)),
            Item::Texture_SRV(3, target(RenderTargetId::GBufferSpecular)),
            Item::Texture_SRV(4, target(RenderTargetId::SceneDepth)),
            Item::Texture_UAV(5, m_guides[0].texture),
            Item::Texture_UAV(6, m_guides[1].texture),
            Item::Texture_UAV(7, m_guides[2].texture),
            Item::Texture_SRV(8, target(RenderTargetId::SceneReflections)),
            Item::Texture_UAV(9, m_guides[kGuideHitDistance].texture),
            Item::Texture_UAV(10, m_guides[kGuideReflectionMotion].texture),
            Item::PushConstants(0, sizeof(DlssMotionPushConstants))};
        m_guideBindingSets.push_back(
            CreateNvrhiBindingSet(m_nvrhiDevice, guideDesc, m_guideSetLayout, "Failed to create a ray reconstruction guide binding set"));
    }
}
}
