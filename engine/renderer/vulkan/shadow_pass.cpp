#include "shadow_pass.h"

#include "parallel_recorder.h"
#include "sampler_settings.h"

#include <engine/asset/mesh.h>
#include <engine/core/log/log.h>
#include <engine/renderer/material.h>

#include <stdexcept>

namespace me
{

namespace
{
// Slope-scaled rasterization bias, applied when the map is rendered. It covers the depth error that
// grows with the surface's slope to the light; the shader's normal offset covers the rest.
constexpr int kDepthBiasConstant = 1;
constexpr float kDepthBiasSlope = 2.0f;

// The depth formats a shadow map may take, best first: the target, sampled, linear comparison filtering
// preferred.
nvrhi::Format ChooseShadowFormat(nvrhi::IDevice* device, bool& linear)
{
    const nvrhi::FormatSupport needed = nvrhi::FormatSupport::DepthStencil | nvrhi::FormatSupport::ShaderSample;
    for (const nvrhi::Format format : {nvrhi::Format::D32, nvrhi::Format::D16})
    {
        const nvrhi::FormatSupport support = device->queryFormatSupport(format);
        if ((support & needed) == needed)
        {
            // With linear filtering a comparison sampler returns the bilinear blend of four
            // comparisons, which is what turns the shader's 3x3 taps into a smooth 4x4 texel filter.
            linear = (support & nvrhi::FormatSupport::ShaderSample) == nvrhi::FormatSupport::ShaderSample;
            return format;
        }
    }
    throw std::runtime_error("The device has no depth format for shadow maps");
}
}

// ---------------------------------------------------------------------------------------------
// ShadowCasterRenderer

ShadowCasterRenderer::ShadowCasterRenderer(
    nvrhi::IDevice* device,
    const nvrhi::FramebufferInfo& framebuffer,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* materialSetLayout,
    const ShadowCasterPipelineDesc& pipelineDesc)
{
    m_constants = CreatePushConstantLayout(device, 2, sizeof(ShadowPushConstants), nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel);
    const nvrhi::ShaderHandle vertexShader = CreateNvrhiShader(device, nvrhi::ShaderType::Vertex, "shadow.vert.spv");
    const nvrhi::ShaderHandle fragmentShader = CreateNvrhiShader(device, nvrhi::ShaderType::Pixel, "shadow.frag.spv");
    const nvrhi::ShaderHandle depthVertexShader = CreateNvrhiShader(device, nvrhi::ShaderType::Vertex, "shadow_depth.vert.spv");

    // Position and both UV sets from the vertex: the alpha test may sample the base colour through
    // either. Opaque casters read positions alone, from their own tightly packed stream.
    const std::array<nvrhi::VertexAttributeDesc, 3> maskAttributes = {
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(static_cast<uint32_t>(offsetof(Vertex, position))).setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(static_cast<uint32_t>(offsetof(Vertex, texCoord))).setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc().setName("SECOND_TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(static_cast<uint32_t>(offsetof(Vertex, texCoord1))).setElementStride(sizeof(Vertex))};
    const nvrhi::VertexAttributeDesc positionAttribute =
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setElementStride(sizeof(float) * 3);
    const nvrhi::InputLayoutHandle maskLayout = device->createInputLayout(maskAttributes.data(), static_cast<uint32_t>(maskAttributes.size()), vertexShader);
    const nvrhi::InputLayoutHandle depthLayout = device->createInputLayout(&positionAttribute, 1, depthVertexShader);
    if (!maskLayout || !depthLayout)
    {
        throw std::runtime_error("Failed to create the shadow caster input layouts");
    }

    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    // No culling: single-sided geometry such as a wall with one face still has to block the light
    // from behind, and the depth bias rather than front-face culling keeps acne off lit surfaces.
    desc.renderState.rasterState.setFillSolid().setCullNone().setFrontCounterClockwise(true);
    desc.renderState.rasterState.setDepthBias(pipelineDesc.depthBias).setSlopeScaleDepthBias(pipelineDesc.slopeScaledDepthBias);
    desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(pipelineDesc.depthFunc).setStencilEnable(false);

    // Opaque casters need no fragment shader: depth is all they write.
    desc.VS = depthVertexShader;
    desc.inputLayout = depthLayout;
    desc.bindingLayouts = {m_constants.layout};
    m_opaquePipeline = device->createGraphicsPipeline(desc, framebuffer);
    desc.VS = vertexShader;
    desc.PS = fragmentShader;
    desc.inputLayout = maskLayout;
    desc.bindingLayouts = {frameSetLayout, materialSetLayout, m_constants.layout};
    m_maskPipeline = device->createGraphicsPipeline(desc, framebuffer);
    if (!m_opaquePipeline || !m_maskPipeline)
    {
        throw std::runtime_error("Failed to create the shadow caster pipelines");
    }
}

void ShadowCasterRenderer::RecordDraw(
    nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* framebuffer,
    const nvrhi::ViewportState& viewport,
    nvrhi::IBindingSet* frameSet,
    const glm::mat4& viewProjection,
    const ShadowDrawItem& item) const
{
    nvrhi::GraphicsState state;
    state.framebuffer = framebuffer;
    state.viewport = viewport;
    if (item.alphaMask)
    {
        state.pipeline = m_maskPipeline;
        state.bindings = {frameSet, item.materialSet, m_constants.set};
        state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(item.vertexBuffer).setSlot(0).setOffset(0)};
    }
    else
    {
        state.pipeline = m_opaquePipeline;
        state.bindings = {m_constants.set};
        state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(item.positionBuffer).setSlot(0).setOffset(0)};
    }
    state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(item.indexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
    commandList->setGraphicsState(state);
    ShadowPushConstants constants{};
    constants.lightModelViewProjection = viewProjection * item.model;
    constants.drawSlot = item.drawSlot;
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(item.indexCount));
}

// ---------------------------------------------------------------------------------------------
// VulkanShadowPass

VulkanShadowPass::VulkanShadowPass(
    nvrhi::IDevice* nvrhiDevice,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* materialSetLayout,
    uint32_t resolution)
    : m_nvrhiDevice(nvrhiDevice),
      m_resolution(resolution)
{
    bool linear = false;
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.width = m_resolution;
    desc.height = m_resolution;
    desc.arraySize = kShadowCascadeCount;
    desc.format = ChooseShadowFormat(m_nvrhiDevice, linear);
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    desc.debugName = "Shadow map";
    // Where the material pass samples it between frames.
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    m_texture = m_nvrhiDevice->createTexture(desc);
    if (!m_texture)
    {
        throw std::runtime_error("Failed to create the shadow map");
    }
    // One single-layer framebuffer per cascade.
    for (uint32_t layer = 0; layer < kShadowCascadeCount; ++layer)
    {
        nvrhi::FramebufferDesc framebuffer;
        framebuffer.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_texture).setArraySlice(layer));
        m_framebuffers[layer] = m_nvrhiDevice->createFramebuffer(framebuffer);
        if (!m_framebuffers[layer])
        {
            throw std::runtime_error("Failed to create a shadow cascade framebuffer");
        }
    }

    nvrhi::SamplerDesc samplerDesc = BuildClampSamplerDesc(linear);
    // Outside the map counts as lit: the border is the far plane, and every receiver passes a
    // LESS_OR_EQUAL comparison against it.
    samplerDesc.addressU = nvrhi::SamplerAddressMode::Border;
    samplerDesc.addressV = nvrhi::SamplerAddressMode::Border;
    samplerDesc.borderColor = nvrhi::Color(1.0f, 1.0f, 1.0f, 1.0f);
    samplerDesc.reductionType = nvrhi::SamplerReductionType::Comparison;
    samplerDesc.comparisonFunc = nvrhi::ComparisonFunc::LessOrEqual;
    m_sampler = CreateNvrhiSampler(m_nvrhiDevice, samplerDesc, "Failed to create shadow map sampler");

    ShadowCasterPipelineDesc casters;
    casters.depthFunc = nvrhi::ComparisonFunc::Less;
    casters.depthBias = kDepthBiasConstant;
    casters.slopeScaledDepthBias = kDepthBiasSlope;
    m_casters = std::make_unique<ShadowCasterRenderer>(
        m_nvrhiDevice, m_framebuffers[0]->getFramebufferInfo(), frameSetLayout, materialSetLayout, casters);
    LOG_INFO("Created a {}x{} shadow map with {} cascades", m_resolution, m_resolution, kShadowCascadeCount);
}

VulkanShadowPass::~VulkanShadowPass() = default;

uint32_t VulkanShadowPass::GetResolution() const
{
    return m_resolution;
}

TextureDescriptorBinding VulkanShadowPass::GetSampledBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_texture, m_sampler);
}

std::optional<ShadowCascadePlan> VulkanShadowPass::Plan(const ShadowCascades* cascades, uint64_t casterKey)
{
    if (cascades == nullptr)
    {
        m_cache.Invalidate();
        m_frameRedraw.fill(!m_cleared);
        m_cleared = true;
        return std::nullopt;
    }
    m_cleared = false;
    const ShadowCascadePlan plan = m_cache.Plan(*cascades, casterKey);
    m_frameRedraw = plan.redraw;
    return plan;
}

void VulkanShadowPass::Record(
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameSet,
    std::span<const ShadowDrawItem> drawItems,
    const ShadowCascadePlan* plan,
    VulkanGpuTimer* timer,
    VulkanParallelRecorder* recorder) const
{
    (void)recorder;
    static constexpr std::array<const char*, kShadowCascadeCount> kCascadeNames = {
        "Shadows/C0", "Shadows/C1", "Shadows/C2", "Shadows/C3"};
    commandList->clearState();
    if (!m_initialized)
    {
        // Out of nothing, once: the material pass samples every layer from the first frame on.
        commandList->setTextureState(m_texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
        m_initialized = true;
    }
    const nvrhi::ViewportState viewport = NativeViewportState(VkExtent2D{m_resolution, m_resolution});
    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount; ++cascadeIndex)
    {
        if (m_frameRedraw[cascadeIndex])
        {
            // Every frame that redraws a layer rewrites the whole of it.
            const nvrhi::TextureSubresourceSet layer(0, 1, cascadeIndex, 1);
            commandList->setTextureState(m_texture, layer, nvrhi::ResourceStates::DepthWrite);
            commandList->commitBarriers();
            commandList->setEnableAutomaticBarriers(true);
            commandList->clearDepthStencilTexture(m_texture, layer, true, 1.0f, false, 0);
            commandList->setEnableAutomaticBarriers(false);
            commandList->setTextureState(m_texture, layer, nvrhi::ResourceStates::DepthWrite);
            commandList->commitBarriers();
            if (plan != nullptr)
            {
                const glm::mat4& lightViewProjection = plan->held[cascadeIndex].viewProjection;
                m_casters->Record(
                    commandList,
                    m_framebuffers[cascadeIndex],
                    viewport,
                    frameSet,
                    lightViewProjection,
                    drawItems,
                    [&lightViewProjection](const ShadowDrawItem& item)
                    {
                        return ShadowCascadeIntersectsSphere(lightViewProjection, item.worldBoundsCenter, item.worldBoundsRadius);
                    });
            }
            // This frame's material pass reads what was written here.
            commandList->setTextureState(m_texture, layer, nvrhi::ResourceStates::ShaderResource);
            commandList->commitBarriers();
            commandList->clearState();
        }
        if (timer != nullptr)
        {
            timer->Mark(kCascadeNames[cascadeIndex]);
        }
    }
}
}
