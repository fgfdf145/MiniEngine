#include "tonemap_pass.h"

#include "gbuffer_inputs.h"
#include "nvrhi_pass.h"
#include "sampler_settings.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
// The tone mapping push constant block. Must match TonemapConstants in shaders/vulkan/tonemap.frag.
struct TonemapPushConstants
{
    uint32_t gbufferView = 0;
    // 1 for HDR10 output (GT7's HDR curve for peakNits), 0 for SDR.
    uint32_t hdrOutput = 0;
    float peakNits = 1000.0f;
    // The operator: one of the kOperator* values below. Must match TONEMAP_OPERATOR_* in the shader.
    uint32_t toneOperator = 0;
    // The white balance matrix's columns, xyz used (see WhiteBalanceMatrix).
    glm::vec4 whiteBalance[3] = {glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)};
    // The HDR calibration (see DisplayOutput).
    float uiWhiteNits = kDefaultUiWhiteNits;
    float blackNits = 0.0f;
    float paperWhiteScale = 1.0f;
    // A CalibrationPattern and its level in cd/m^2.
    uint32_t pattern = 0;
    float patternLevel = 0.0f;
    float padding[3] = {};
};

static_assert(sizeof(TonemapPushConstants) == 96, "TonemapPushConstants must match the shader's block");

constexpr uint32_t kOperatorGt7 = 0;
// The Khronos reference view: PBR Neutral plus the Sample Viewer's 2.2 gamma on an SDR display.
constexpr uint32_t kOperatorKhronosReference = 1;
constexpr uint32_t kOperatorPbrNeutral = 2;
constexpr uint32_t kOperatorNone = 3;

uint32_t ToneOperator(const ScenePassFrameContext& frame)
{
    if (frame.khronosReference)
    {
        return kOperatorKhronosReference;
    }
    switch (frame.toneMapper)
    {
    case ToneMapper::PbrNeutral:
        return kOperatorPbrNeutral;
    case ToneMapper::None:
        return kOperatorNone;
    case ToneMapper::Gt7:
    default:
        return kOperatorGt7;
    }
}
}

VulkanTonemapPass::VulkanTonemapPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* gbufferSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    // The pass samples one texel per pixel at matching resolution, so linear filtering would only
    // blur, and with one mip level there is nothing for a mip mode or a LOD range to select.
    m_sampler = CreateNvrhiSampler(nvrhiDevice, BuildClampSamplerDesc(false), "Failed to create tone mapping sampler");

    // The view changes whenever the user picks one and the white balance every frame, so both are
    // push constants rather than something that would force the binding sets to be rebuilt.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = ShaderBindingOffsets();
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(64),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(TonemapPushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, layoutDesc, "Failed to create the tone mapping binding layout");
    CreateBindingSets(targets);
    CreateFramebuffers(targets);

    // Every channel written without blending: the LDR alpha must stay 1.0 for ImGui.
    nvrhi::GraphicsPipelineDesc desc;
    desc.VS = CreateNvrhiShader(m_nvrhiDevice, nvrhi::ShaderType::Vertex, "fullscreen.vert.spv");
    desc.PS = CreateNvrhiShader(m_nvrhiDevice, nvrhi::ShaderType::Pixel, "tonemap.frag.spv");
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.bindingLayouts = {m_setLayout, gbufferSetLayout};
    desc.renderState.rasterState.setCullNone();
    desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
    m_pipeline = m_nvrhiDevice->createGraphicsPipeline(desc, m_framebuffers.front());
    if (!m_pipeline)
    {
        throw std::runtime_error("Failed to create the tone mapping pipeline");
    }
}

VulkanTonemapPass::~VulkanTonemapPass() = default;

ScenePassId VulkanTonemapPass::Id() const
{
    return ScenePassId::Tonemap;
}

RenderPassIo VulkanTonemapPass::Io() const
{
    // Binding set 2 requires every image in it to be in the read layout whenever this pass
    // records, whether or not the selected view samples it, so all thirteen G-buffer inputs are
    // declared reads alongside the HDR target. In an order that never wrote them their contents
    // are undefined, and the renderer forces the view off.
    static constexpr std::array<RenderTargetId, 15> kReads = {
        RenderTargetId::SceneTaa,
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::SceneAo,
        RenderTargetId::GBufferSpecular,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferSheen,
        RenderTargetId::SceneReflections,
        RenderTargetId::SceneGi,
        RenderTargetId::SceneShadow,
        RenderTargetId::ScenePathTrace};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneLdr};

    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanTonemapPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    // There is one framebuffer per LDR copy and one binding set per HDR copy, so both indices come
    // from the target they belong to rather than from a rule repeated here. The layout tracker put
    // the LDR target in COLOR_ATTACHMENT_OPTIMAL (the write); the full-screen triangle covers every
    // pixel and writes all four channels.
    const uint32_t ldrSlot = targets.ResolveIndex(RenderTargetId::SceneLdr, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(commandList, {{targets.GetTexture(RenderTargetId::SceneLdr, ldrSlot), nvrhi::ResourceStates::RenderTarget}});
    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = m_framebuffers.at(ldrSlot);
    state.viewport = NativeViewportState(frame.outputExtent);
    state.bindings = {m_bindingSets.at(targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot)), frame.gbufferBindingSet};
    commandList->setGraphicsState(state);

    TonemapPushConstants constants{};
    constants.gbufferView = static_cast<uint32_t>(frame.gbufferView);
    constants.hdrOutput = frame.display.hdr ? 1u : 0u;
    constants.peakNits = frame.display.maxLuminance;
    constants.toneOperator = ToneOperator(frame);
    for (int column = 0; column < 3; ++column)
    {
        constants.whiteBalance[column] = glm::vec4(frame.whiteBalance[column], 0.0f);
    }
    constants.uiWhiteNits = frame.display.uiWhiteNits;
    constants.blackNits = frame.display.minLuminance;
    // The Khronos reference view renders as the Sample Viewer does: no paper white lift.
    constants.paperWhiteScale = frame.khronosReference ? 1.0f : HdrPaperWhiteScale(frame.display);
    constants.pattern = static_cast<uint32_t>(frame.calibrationView.pattern);
    constants.patternLevel = frame.calibrationView.level;
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}

void VulkanTonemapPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // Both sides of the pass follow the new images: the framebuffers attach the new LDR images and
    // the binding sets sample the new HDR ones. The pipeline depends only on the LDR format, which a
    // rebuild does not change.
    CreateFramebuffers(targets);
    CreateBindingSets(targets);
}

void VulkanTonemapPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    // One set per HDR copy, so these are indexed by frame slot.
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SceneTaa, slot)),
            nvrhi::BindingSetItem::Sampler(64, m_sampler),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(TonemapPushConstants))};
        m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a tone mapping binding set"));
    }
}

void VulkanTonemapPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    // One framebuffer per LDR copy, so these are indexed by swapchain image.
    m_framebuffers.clear();
    for (uint32_t imageIndex = 0; imageIndex < targets.GetLdrCopyCount(); ++imageIndex)
    {
        nvrhi::FramebufferHandle framebuffer = m_nvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(targets.GetTexture(RenderTargetId::SceneLdr, imageIndex)));
        if (!framebuffer)
        {
            throw std::runtime_error("Failed to create a tone mapping framebuffer");
        }
        m_framebuffers.push_back(framebuffer);
    }
}
}
