#include "lighting_pass.h"

#include "gbuffer_inputs.h"
#include "nvrhi_pass.h"
#include "pipeline.h"
#include "ray_scene.h"

#include <array>
#include <stdexcept>

namespace me
{

namespace
{
// Must match LightingConstants in shaders/vulkan/deferred_lighting.frag.
struct LightingPushConstants
{
    // xyz = the background radiance, w unused.
    glm::vec4 backgroundRadiance{0.0f};
    // x = 1 to draw the light cluster heat map instead of shading; y = 1 to read the ray traced sun
    // shadow (SceneShadow) instead of the cascades; z = 1 to trace the local lights' shadows (the ray
    // query pipeline); w = 1 for path tracing mode (the traced light in SceneGi and SceneReflections
    // in place of the ambient terms), 2 for ReSTIR PT (ScenePathTrace in place of all the shading).
    glm::vec4 debug{0.0f};
};

static_assert(sizeof(LightingPushConstants) == 32, "LightingPushConstants must match the shader's block");

// deferred_lighting.frag has no set of its own: its push constants are the only thing in register space
// 4 (docs/design/2026-10-09-dxil-shader-portability-design.md), a set both variants leave free.
constexpr uint32_t kPushConstantSpace = 4;
}

VulkanLightingPass::VulkanLightingPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* gbufferSetLayout,
    const VulkanRayScene& rayScene)
    : m_nvrhiDevice(nvrhiDevice)
{
    CreateFramebuffers(targets);
    nvrhi::BindingLayoutDesc pushDesc;
    pushDesc.visibility = nvrhi::ShaderType::Pixel;
    pushDesc.registerSpace = kPushConstantSpace;
    pushDesc.registerSpaceIsDescriptorSet = true;
    pushDesc.bindingOffsets = ShaderBindingOffsets();
    pushDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(LightingPushConstants))};
    m_pushLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, pushDesc, "Failed to create the lighting push constant layout");
    nvrhi::BindingSetDesc pushSet;
    pushSet.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(LightingPushConstants))};
    m_pushSet = CreateNvrhiBindingSet(m_nvrhiDevice, pushSet, m_pushLayout, "Failed to create the lighting push constant set");

    const auto pipeline = [&](const char* shader, std::initializer_list<nvrhi::IBindingLayout*> layouts)
    {
        nvrhi::GraphicsPipelineDesc desc;
        desc.VS = CreateNvrhiShader(m_nvrhiDevice, nvrhi::ShaderType::Vertex, "fullscreen.vert.spv");
        desc.PS = CreateNvrhiShader(m_nvrhiDevice, nvrhi::ShaderType::Pixel, shader);
        desc.primType = nvrhi::PrimitiveType::TriangleList;
        for (nvrhi::IBindingLayout* layout : layouts)
        {
            desc.bindingLayouts.push_back(layout);
        }
        desc.renderState.rasterState.setCullNone();
        desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        nvrhi::GraphicsPipelineHandle result = m_nvrhiDevice->createGraphicsPipeline(desc, m_framebuffers.front());
        if (!result)
        {
            throw std::runtime_error(std::string("Failed to create the lighting pipeline for ") + shader);
        }
        return result;
    };
    m_pipeline = pipeline("deferred_lighting.frag.spv", {frameSetLayout, gbufferSetLayout, m_pushLayout});
    if (rayScene.HasHardwareRayTracing())
    {
        // Its set 1 is the ray scene and set 3 its texture table.
        m_tracedPipeline = pipeline(
            "deferred_lighting_ray_query.frag.spv",
            {frameSetLayout, rayScene.GetNvrhiSetLayout(), gbufferSetLayout, rayScene.GetNvrhiTextureSetLayout(), m_pushLayout});
    }
}

VulkanLightingPass::~VulkanLightingPass() = default;

ScenePassId VulkanLightingPass::Id() const
{
    return ScenePassId::Lighting;
}

RenderPassIo VulkanLightingPass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneHdr};

    RenderPassIo io{};
    io.reads = VulkanGBufferDescriptors::kInputs;
    io.writes = kWrites;
    return io;
}

void VulkanLightingPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    // The triangle writes every pixel, background included: nothing is cleared. The layout tracker put
    // the HDR target in COLOR_ATTACHMENT_OPTIMAL (the write) and the G-buffer in the read layout.
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(commandList, {{targets.GetTexture(RenderTargetId::SceneHdr, slot), nvrhi::ResourceStates::RenderTarget}});
    // The ray query variant traces the local lights' shadows.
    const bool traced = frame.rayTracing.localShadows && m_tracedPipeline && frame.rayBindingSet != nullptr && frame.rayTextureTable != nullptr;
    nvrhi::GraphicsState state;
    state.framebuffer = m_framebuffers.at(slot);
    state.viewport = NativeViewportState(frame.extent);
    if (traced)
    {
        state.pipeline = m_tracedPipeline;
        state.bindings = {frame.frameBindingSet, frame.rayBindingSet, frame.gbufferBindingSet, frame.rayTextureTable, m_pushSet};
    }
    else
    {
        state.pipeline = m_pipeline;
        state.bindings = {frame.frameBindingSet, frame.gbufferBindingSet, m_pushSet};
    }
    commandList->setGraphicsState(state);

    LightingPushConstants constants{};
    // The same constant the forward pass clears with, so the two orders' backgrounds cannot differ.
    constants.backgroundRadiance = glm::vec4(kViewportBackgroundFrameBuffer, 1.0f);
    constants.debug = glm::vec4(
        frame.gbufferView == GBufferDebugView::LightClusters ? 1.0f : 0.0f,
        frame.rayTracing.sunShadows ? 1.0f : 0.0f,
        traced ? 1.0f : 0.0f,
        frame.pathTracing.enabled ? (frame.pathTracing.restir ? 2.0f : (frame.pathTracing.offline.enabled ? 3.0f : 1.0f)) : 0.0f);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}

void VulkanLightingPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The pipelines depend only on the HDR format, which a rebuild never changes.
    CreateFramebuffers(targets);
}

void VulkanLightingPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::FramebufferHandle framebuffer = m_nvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(targets.GetTexture(RenderTargetId::SceneHdr, slot)));
        if (!framebuffer)
        {
            throw std::runtime_error("Failed to create a lighting framebuffer");
        }
        m_framebuffers.push_back(framebuffer);
    }
}
}
