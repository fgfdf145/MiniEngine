#include "toon_pass.h"

#include "compute_pass_util.h"
#include "nvrhi_pass.h"
#include "pipeline_set.h"
#include "reverse_depth.h"

#include <engine/core/log/log.h>

#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
// The linear depth the prepass clears to: farther than any character, so where none is, the rim light
// sees background.
constexpr float kToonFarDepth = 1.0e6f;

// Which surface pipeline a toon draw takes: transparent ones blend (and leave the eye mask alone in
// the prepass), double-sided ones cull nothing.
size_t ToonPipelineIndex(const VulkanDrawItem& item)
{
    const bool transparent = item.toon->Has(kToonFeatureTransparent);
    return (transparent ? 2u : 0u) + (item.pipelineKey.doubleSided ? 1u : 0u);
}
constexpr size_t kOutlinePipeline = 4;

// How one toon pipeline differs from the next.
struct ToonPipelineDescription
{
    nvrhi::IShader* vertexShader = nullptr;
    nvrhi::IShader* fragmentShader = nullptr;
    nvrhi::RasterCullMode cullMode = nvrhi::RasterCullMode::Back;
    nvrhi::ComparisonFunc depthCompare = nvrhi::ComparisonFunc::GreaterOrEqual;
    // Pulls the surface a hair toward the camera, so that a toon draw lands on the depth the
    // geometry pass wrote for it from triangle.vert.
    bool depthBias = false;
    bool blend = false;
    // Per colour target: write it, or leave it as it is.
    std::array<bool, 2> writeAttachment = {true, true};
    // The prepass's targets: one channel each.
    bool singleChannel = false;
};

nvrhi::GraphicsPipelineHandle CreateToonPipeline(
    nvrhi::IDevice* device,
    const nvrhi::FramebufferInfo& framebuffer,
    nvrhi::IInputLayout* inputLayout,
    std::initializer_list<nvrhi::IBindingLayout*> layouts,
    const ToonPipelineDescription& description,
    const char* label)
{
    nvrhi::GraphicsPipelineDesc desc;
    desc.VS = description.vertexShader;
    desc.PS = description.fragmentShader;
    desc.inputLayout = inputLayout;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    for (nvrhi::IBindingLayout* layout : layouts)
    {
        desc.bindingLayouts.push_back(layout);
    }
    // As the material pipelines (VulkanPipelineSet). Reverse-Z: a positive bias moves toward the camera.
    desc.renderState.rasterState.setFillSolid().setFrontCounterClockwise(true).setCullMode(description.cullMode);
    if (description.depthBias)
    {
        desc.renderState.rasterState.setDepthBias(4).setSlopeScaleDepthBias(1.0f);
    }
    desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(description.depthCompare).setStencilEnable(false);
    for (uint32_t index = 0; index < framebuffer.colorFormats.size(); ++index)
    {
        // The toon pass's two targets take rgb alike (the HDR target's alpha keeps its clear value, as
        // the forward pass leaves it; the velocity's b is the coat normal no toon surface has). The
        // prepass writes its single-channel targets' r.
        nvrhi::BlendState::RenderTarget& target = desc.renderState.blendState.targets[index];
        const nvrhi::ColorMask channels = description.singleChannel
                                              ? nvrhi::ColorMask::Red
                                              : nvrhi::ColorMask(nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue);
        target.setColorWriteMask(description.writeAttachment[index] ? channels : nvrhi::ColorMask(0));
        if (description.blend)
        {
            target.setBlendEnable(true)
                .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOp(nvrhi::BlendOp::Add)
                .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                .setDestBlendAlpha(nvrhi::BlendFactor::One)
                .setBlendOpAlpha(nvrhi::BlendOp::Add);
        }
    }
    nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(desc, framebuffer);
    if (!pipeline)
    {
        throw std::runtime_error(label);
    }
    return pipeline;
}

// Constant 0 of both stages: the outline (toon.vert pushes the hull out, toon.frag colours it).
nvrhi::ShaderHandle ToonShader(nvrhi::IDevice* device, nvrhi::ShaderType type, const char* name, bool outline)
{
    const nvrhi::ShaderHandle shader = CreateNvrhiShader(device, type, name);
    const std::array<nvrhi::ShaderSpecialization, 1> constants = {nvrhi::ShaderSpecialization::UInt32(0, outline ? 1u : 0u)};
    return SpecializeShader(device, shader, constants, name);
}

// Draws a toon draw: its geometry, its material set and its constants, over state's pipeline, targets
// and the other sets.
void RecordToonDraw(
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    const VulkanDrawItem& item,
    uint32_t toonIndex,
    float exposureScale)
{
    state.bindings[1] = item.materialSet;
    state.vertexBuffers[0].setBuffer(item.vertexBuffer);
    state.vertexBuffers[1].setBuffer(item.previousPositionBuffer);
    state.indexBuffer.setBuffer(item.indexBuffer);
    commandList->setGraphicsState(state);
    ToonPushConstants constants{};
    constants.model = item.drawConstants.model;
    constants.toonIndex = toonIndex;
    constants.exposureScale = exposureScale;
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(item.indexCount).setStartInstanceLocation(item.motionSlot));
}

nvrhi::GraphicsState ToonState(nvrhi::IFramebuffer* framebuffer, VkExtent2D extent)
{
    nvrhi::GraphicsState state;
    state.framebuffer = framebuffer;
    state.viewport = NativeViewportState(extent);
    state.vertexBuffers = {nvrhi::VertexBufferBinding().setSlot(0).setOffset(0), nvrhi::VertexBufferBinding().setSlot(1).setOffset(0)};
    state.indexBuffer.setFormat(nvrhi::Format::R32_UINT).setOffset(0);
    return state;
}
}

// ---------------------------------------------------------------------------------------------
// VulkanToonMaterials

VulkanToonMaterials::VulkanToonMaterials(nvrhi::IDevice* nvrhiDevice, uint32_t frameSlotCount)
{
    // The materials (binding 0) and every toon pipeline's push constants, in register space 2.
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    desc.registerSpace = 2;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0), nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ToonPushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(nvrhiDevice, desc, "Failed to create the toon material binding layout");
    const uint64_t bytes = sizeof(GpuToonMaterial) * kMaxDraws;
    for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
    {
        void* mapped = nullptr;
        m_buffers.push_back(CreateUploadBuffer(nvrhiDevice, bytes, sizeof(GpuToonMaterial), "Toon materials", &mapped));
        m_mapped.push_back(mapped);
        nvrhi::BindingSetDesc set;
        set.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_buffers.back()),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(ToonPushConstants))};
        m_sets.push_back(CreateNvrhiBindingSet(nvrhiDevice, set, m_setLayout, "Failed to create a toon material binding set"));
    }
}

VulkanToonMaterials::~VulkanToonMaterials() = default;

uint32_t VulkanToonMaterials::Write(uint32_t frameSlot, std::span<const VulkanDrawItem> toonDrawItems, std::span<const glm::mat4> headPoses)
{
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(toonDrawItems.size(), kMaxDraws));
    auto* materials = static_cast<GpuToonMaterial*>(m_mapped.at(frameSlot));
    for (uint32_t index = 0; index < count; ++index)
    {
        GpuToonMaterial material = toonDrawItems[index].toon->gpu;
        if (index < headPoses.size())
        {
            const glm::mat4& pose = headPoses[index];
            const glm::vec3 position = glm::vec3(pose * glm::vec4(material.headPosition[0], material.headPosition[1], material.headPosition[2], 1.0f));
            const glm::vec3 forward = glm::normalize(glm::mat3(pose) * glm::vec3(material.headForward[0], material.headForward[1], material.headForward[2]));
            const glm::vec3 up = glm::normalize(glm::mat3(pose) * glm::vec3(material.headUp[0], material.headUp[1], material.headUp[2]));
            std::copy_n(&position.x, 3, material.headPosition);
            std::copy_n(&forward.x, 3, material.headForward);
            std::copy_n(&up.x, 3, material.headUp);
        }
        // The draw's material texture samplers: the toon shaders know the draw by its index here.
        std::copy(toonDrawItems[index].samplerIndices.begin(), toonDrawItems[index].samplerIndices.end(), material.samplerIndices);
        std::memcpy(&materials[index], &material, sizeof(GpuToonMaterial));
    }
    return count;
}

nvrhi::IBindingLayout* VulkanToonMaterials::GetSetLayout() const
{
    return m_setLayout;
}

nvrhi::IBindingSet* VulkanToonMaterials::GetSet(uint32_t frameSlot) const
{
    return m_sets.at(frameSlot);
}

// ---------------------------------------------------------------------------------------------
// VulkanToonPrepass

VulkanToonPrepass::VulkanToonPrepass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* materialSetLayout,
    const VulkanToonMaterials& materials)
    : m_nvrhiDevice(nvrhiDevice),
      m_materials(materials)
{
    CreateFramebuffers(targets);
    const nvrhi::ShaderHandle vertexShader = ToonShader(m_nvrhiDevice, nvrhi::ShaderType::Vertex, "toon.vert.spv", false);
    const nvrhi::ShaderHandle fragmentShader = ToonShader(m_nvrhiDevice, nvrhi::ShaderType::Pixel, "toon_prepass.frag.spv", false);
    const nvrhi::InputLayoutHandle inputLayout = CreateMaterialInputLayout(m_nvrhiDevice, vertexShader);
    for (size_t index = 0; index < m_pipelines.size(); ++index)
    {
        const bool transparent = index >= 2;
        ToonPipelineDescription description{};
        description.vertexShader = vertexShader;
        description.fragmentShader = fragmentShader;
        description.cullMode = (index % 2) == 1 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
        description.depthCompare = nvrhi::ComparisonFunc::Greater;
        description.singleChannel = true;
        // A transparent surface adds its depth but leaves the eye mask to the opaque ones.
        description.writeAttachment = {true, !transparent};
        m_pipelines[index] = CreateToonPipeline(
            m_nvrhiDevice,
            m_framebuffers.front()->getFramebufferInfo(),
            inputLayout,
            {frameSetLayout, materialSetLayout, m_materials.GetSetLayout()},
            description,
            "Failed to create a toon prepass pipeline");
    }
}

VulkanToonPrepass::~VulkanToonPrepass() = default;

ScenePassId VulkanToonPrepass::Id() const
{
    return ScenePassId::ToonPrepass;
}

RenderPassIo VulkanToonPrepass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kWrites = {
        RenderTargetId::ToonLinearDepth,
        RenderTargetId::ToonMask,
        RenderTargetId::ToonDepth};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanToonPrepass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::ToonDepth, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    nvrhi::ITexture* linearDepth = targets.GetTexture(RenderTargetId::ToonLinearDepth, slot);
    nvrhi::ITexture* mask = targets.GetTexture(RenderTargetId::ToonMask, slot);
    nvrhi::ITexture* depth = targets.GetTexture(RenderTargetId::ToonDepth, slot);
    const NvrhiPassScope scope(
        commandList,
        {{linearDepth, nvrhi::ResourceStates::RenderTarget}, {mask, nvrhi::ResourceStates::RenderTarget}, {depth, nvrhi::ResourceStates::DepthWrite}});
    // Cleared every frame, characters or none: the toon pass reads them either way.
    ClearTextureFloat(commandList, linearDepth, nvrhi::Color(kToonFarDepth, 0.0f, 0.0f, 0.0f));
    ClearTextureFloat(commandList, mask, nvrhi::Color(0.0f));
    ClearDepth(commandList, depth, kReverseDepthFar);
    commandList->setTextureState(linearDepth, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->setTextureState(mask, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->setTextureState(depth, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    commandList->commitBarriers();
    if (frame.toonDrawItems.empty())
    {
        return;
    }
    nvrhi::GraphicsState state = ToonState(m_framebuffers.at(slot), frame.extent);
    state.bindings = {frame.frameBindingSet, nullptr, m_materials.GetSet(frame.frameSlot)};
    // The opaque draws come first in the list, so the mask holds the nearest opaque surface before the
    // transparent ones add their depth.
    for (uint32_t index = 0; index < frame.toonDrawItems.size(); ++index)
    {
        const VulkanDrawItem& item = frame.toonDrawItems[index];
        state.pipeline = m_pipelines[ToonPipelineIndex(item)];
        RecordToonDraw(commandList, state, item, index, frame.toonExposureScale);
    }
}

void VulkanToonPrepass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
}

void VulkanToonPrepass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        m_framebuffers.push_back(CreateNvrhiFramebuffer(
            m_nvrhiDevice,
            {targets.GetTexture(RenderTargetId::ToonLinearDepth, slot), targets.GetTexture(RenderTargetId::ToonMask, slot)},
            targets.GetTexture(RenderTargetId::ToonDepth, slot)));
    }
}

// ---------------------------------------------------------------------------------------------
// VulkanToonPass

VulkanToonPass::VulkanToonPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* materialSetLayout,
    const VulkanToonMaterials& materials)
    : m_nvrhiDevice(nvrhiDevice),
      m_materials(materials)
{
    CreateFramebuffers(targets);
    // Set 3: the prepass's linear depth and eye mask, which the shader fetches by index.
    nvrhi::BindingLayoutDesc targetDesc;
    targetDesc.visibility = nvrhi::ShaderType::Pixel;
    targetDesc.registerSpace = 3;
    targetDesc.registerSpaceIsDescriptorSet = true;
    targetDesc.bindingOffsets = ShaderBindingOffsets();
    targetDesc.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
    m_targetSetLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, targetDesc, "Failed to create the toon target binding layout");
    CreateTargetSets(targets);

    const nvrhi::ShaderHandle vertexShader = ToonShader(m_nvrhiDevice, nvrhi::ShaderType::Vertex, "toon.vert.spv", false);
    const nvrhi::ShaderHandle fragmentShader = ToonShader(m_nvrhiDevice, nvrhi::ShaderType::Pixel, "toon.frag.spv", false);
    const nvrhi::ShaderHandle outlineVertexShader = ToonShader(m_nvrhiDevice, nvrhi::ShaderType::Vertex, "toon.vert.spv", true);
    const nvrhi::ShaderHandle outlineFragmentShader = ToonShader(m_nvrhiDevice, nvrhi::ShaderType::Pixel, "toon.frag.spv", true);
    const nvrhi::InputLayoutHandle inputLayout = CreateMaterialInputLayout(m_nvrhiDevice, vertexShader);
    const nvrhi::FramebufferInfo& framebuffer = m_framebuffers.front()->getFramebufferInfo();
    for (size_t index = 0; index < 4; ++index)
    {
        const bool transparent = index >= 2;
        ToonPipelineDescription description{};
        description.vertexShader = vertexShader;
        description.fragmentShader = fragmentShader;
        description.cullMode = (index % 2) == 1 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
        // The opaque surfaces land on the depth the geometry pass wrote for them (the forward-only order
        // has none, and they write their own); the transparent ones test against everything.
        description.depthCompare = nvrhi::ComparisonFunc::GreaterOrEqual;
        description.depthBias = !transparent;
        description.blend = transparent;
        m_pipelines[index] = CreateToonPipeline(
            m_nvrhiDevice,
            framebuffer,
            inputLayout,
            {frameSetLayout, materialSetLayout, m_materials.GetSetLayout(), m_targetSetLayout},
            description,
            "Failed to create a toon pipeline");
    }
    // The outline: the hull's back faces, behind the surface wherever the surface faces the camera.
    ToonPipelineDescription outline{};
    outline.vertexShader = outlineVertexShader;
    outline.fragmentShader = outlineFragmentShader;
    outline.cullMode = nvrhi::RasterCullMode::Front;
    outline.depthCompare = nvrhi::ComparisonFunc::Greater;
    m_pipelines[kOutlinePipeline] = CreateToonPipeline(
        m_nvrhiDevice,
        framebuffer,
        inputLayout,
        {frameSetLayout, materialSetLayout, m_materials.GetSetLayout(), m_targetSetLayout},
        outline,
        "Failed to create the toon outline pipeline");
}

VulkanToonPass::~VulkanToonPass() = default;

ScenePassId VulkanToonPass::Id() const
{
    return ScenePassId::Toon;
}

RenderPassIo VulkanToonPass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {RenderTargetId::ToonLinearDepth, RenderTargetId::ToonMask};
    // The velocity too: the transparent surfaces (the front hair) are in no G-buffer, so their motion
    // goes in here for TAA and DLSS.
    static constexpr std::array<RenderTargetId, 3> kWrites = {
        RenderTargetId::SceneHdr, RenderTargetId::SceneDepth, RenderTargetId::GBufferVelocity};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanToonPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    if (frame.toonDrawItems.empty())
    {
        return;
    }
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{targets.GetTexture(RenderTargetId::SceneHdr, slot), nvrhi::ResourceStates::RenderTarget},
         {targets.GetTexture(RenderTargetId::GBufferVelocity, slot), nvrhi::ResourceStates::RenderTarget},
         {targets.GetTexture(RenderTargetId::SceneDepth, slot), nvrhi::ResourceStates::DepthWrite}});
    nvrhi::GraphicsState state = ToonState(m_framebuffers.at(slot), frame.extent);
    state.bindings = {frame.frameBindingSet, nullptr, m_materials.GetSet(frame.frameSlot), m_targetSets.at(slot)};
    const auto draw = [&](size_t pipeline, uint32_t index)
    {
        state.pipeline = m_pipelines[pipeline];
        RecordToonDraw(commandList, state, frame.toonDrawItems[index], index, frame.toonExposureScale);
    };
    // The list is the opaque draws, then the transparent ones, each in render queue order.
    uint32_t firstTransparent = 0;
    while (firstTransparent < frame.toonDrawItems.size() && !frame.toonDrawItems[firstTransparent].toon->Has(kToonFeatureTransparent))
    {
        ++firstTransparent;
    }
    for (uint32_t index = 0; index < firstTransparent; ++index)
    {
        draw(ToonPipelineIndex(frame.toonDrawItems[index]), index);
    }
    // Every outline after the opaque surfaces, as the renderer feature that draws Unity's outline pass
    // does; the transparent surfaces then cover theirs where they are nearer.
    for (uint32_t index = 0; index < frame.toonDrawItems.size(); ++index)
    {
        if (frame.toonDrawItems[index].toon->Has(kToonFeatureOutline))
        {
            draw(kOutlinePipeline, index);
        }
    }
    for (uint32_t index = firstTransparent; index < frame.toonDrawItems.size(); ++index)
    {
        draw(ToonPipelineIndex(frame.toonDrawItems[index]), index);
    }
}

void VulkanToonPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
    CreateTargetSets(targets);
}

void VulkanToonPass::CreateTargetSets(const SceneRenderTargets& targets)
{
    m_targetSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::ToonLinearDepth, slot)),
            nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::ToonMask, slot))};
        m_targetSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_targetSetLayout, "Failed to create a toon target binding set"));
    }
}

void VulkanToonPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        m_framebuffers.push_back(CreateNvrhiFramebuffer(
            m_nvrhiDevice,
            {targets.GetTexture(RenderTargetId::SceneHdr, slot), targets.GetTexture(RenderTargetId::GBufferVelocity, slot)},
            targets.GetTexture(RenderTargetId::SceneDepth, slot)));
    }
}
}
