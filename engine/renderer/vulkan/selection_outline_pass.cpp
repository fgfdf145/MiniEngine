#include "selection_outline_pass.h"

#include "nvrhi_pass.h"
#include "reverse_depth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace me
{

namespace
{
// The outline pass's push constant block. Must match OutlineConstants in
// shaders/vulkan/selection_outline.frag.
struct SelectionOutlinePushConstants
{
    // Linear rgb; a is unused.
    glm::vec4 color{0.0f};
    float width = 2.0f;
    // The line's opacity where something nearer hides the entity.
    float occludedOpacity = 0.35f;
    // How far, in pixels, the shader looks for the silhouette: ceil(width).
    int32_t radius = 2;
    // 0 without a selection: the shader writes transparent pixels and samples nothing.
    uint32_t enabled = 0;
};

static_assert(sizeof(SelectionOutlinePushConstants) == 32, "SelectionOutlinePushConstants must match the shader's block");

// Blender's default theme: the active object's outline (#FFAA40), and how much of it shows through
// what hides the object in solid mode (overlay_outline.cc's alpha_occlu).
constexpr std::array<uint8_t, 3> kActiveObjectSrgb = {0xFF, 0xAA, 0x40};
constexpr float kOccludedOpacity = 0.35f;

float SrgbToLinear(uint8_t value)
{
    const float encoded = static_cast<float>(value) / 255.0f;
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}
}

VulkanSelectionMaskPass::VulkanSelectionMaskPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* materialSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    CreateFramebuffers(targets);
    // The scene's reverse-Z (nearer is greater), no bias.
    ShadowCasterPipelineDesc casters;
    casters.depthFunc = nvrhi::ComparisonFunc::Greater;
    m_casters = std::make_unique<ShadowCasterRenderer>(
        m_nvrhiDevice, m_framebuffers.front()->getFramebufferInfo(), frameSetLayout, materialSetLayout, casters);
}

VulkanSelectionMaskPass::~VulkanSelectionMaskPass() = default;

ScenePassId VulkanSelectionMaskPass::Id() const
{
    return ScenePassId::SelectionMask;
}

RenderPassIo VulkanSelectionMaskPass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SelectionDepth};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanSelectionMaskPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SelectionDepth, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    nvrhi::ITexture* depth = targets.GetTexture(RenderTargetId::SelectionDepth, slot);
    const NvrhiPassScope scope(commandList, {{depth, nvrhi::ResourceStates::DepthWrite}});
    // Cleared every frame, selection or none: the outline pass reads it either way.
    ClearDepth(commandList, depth, kReverseDepthFar);
    commandList->setTextureState(depth, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    commandList->commitBarriers();
    m_casters->Record(
        commandList,
        m_framebuffers.at(slot),
        NativeViewportState(frame.outputExtent),
        frame.frameBindingSet,
        frame.selectionViewProjection,
        frame.selectionDrawItems,
        [](const ShadowDrawItem&)
        {
            return true;
        });
}

void VulkanSelectionMaskPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
}

void VulkanSelectionMaskPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        m_framebuffers.push_back(CreateNvrhiFramebuffer(m_nvrhiDevice, {}, targets.GetTexture(RenderTargetId::SelectionDepth, slot)));
    }
}

VulkanSelectionOutlinePass::VulkanSelectionOutlinePass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets)
    : m_nvrhiDevice(nvrhiDevice)
{
    CreateFramebuffers(targets);
    // The selection's depth and the scene's, fetched by texel; the push constants in the same space.
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Pixel;
    desc.registerSpace = 0;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SelectionOutlinePushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the selection outline binding layout");
    m_pipeline = CreateFullscreenNvrhiPipeline(m_nvrhiDevice, m_framebuffers.front()->getFramebufferInfo(), {m_setLayout}, "selection_outline.frag.spv");
    CreateBindingSets(targets);
}

VulkanSelectionOutlinePass::~VulkanSelectionOutlinePass() = default;

ScenePassId VulkanSelectionOutlinePass::Id() const
{
    return ScenePassId::SelectionOutline;
}

RenderPassIo VulkanSelectionOutlinePass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {RenderTargetId::SelectionDepth, RenderTargetId::SceneDepth};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SelectionOutline};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanSelectionOutlinePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    // No clear: the full-screen triangle writes every pixel, transparent where there is no line.
    const uint32_t outlineIndex = targets.ResolveIndex(RenderTargetId::SelectionOutline, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(commandList, {{targets.GetTexture(RenderTargetId::SelectionOutline, outlineIndex), nvrhi::ResourceStates::RenderTarget}});
    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = m_framebuffers.at(outlineIndex);
    state.viewport = NativeViewportState(frame.outputExtent);
    state.bindings = {m_bindingSets.at(targets.ResolveIndex(RenderTargetId::SelectionDepth, frame.imageIndex, frame.frameSlot))};
    commandList->setGraphicsState(state);

    SelectionOutlinePushConstants constants{};
    constants.color = glm::vec4(
        SrgbToLinear(kActiveObjectSrgb[0]),
        SrgbToLinear(kActiveObjectSrgb[1]),
        SrgbToLinear(kActiveObjectSrgb[2]),
        1.0f);
    constants.width = std::max(frame.selectionOutlineWidth, 1.0f);
    constants.occludedOpacity = kOccludedOpacity;
    constants.radius = static_cast<int32_t>(std::ceil(constants.width));
    constants.enabled = frame.selectionDrawItems.empty() ? 0u : 1u;
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}

void VulkanSelectionOutlinePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
    CreateBindingSets(targets);
}

void VulkanSelectionOutlinePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SelectionDepth, slot)),
            nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(SelectionOutlinePushConstants))};
        m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a selection outline binding set"));
    }
}

void VulkanSelectionOutlinePass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t index = 0; index < targets.GetLdrCopyCount(); ++index)
    {
        m_framebuffers.push_back(CreateNvrhiFramebuffer(m_nvrhiDevice, {targets.GetTexture(RenderTargetId::SelectionOutline, index)}));
    }
}
}
