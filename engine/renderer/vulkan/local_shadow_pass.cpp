#include "local_shadow_pass.h"

#include "sampler_settings.h"

#include <engine/core/log/log.h>

#include <stdexcept>

namespace me
{

namespace
{
// The cascades' slope-scaled rasterization bias; the shader's normal offset covers the rest.
constexpr int kDepthBiasConstant = 1;
constexpr float kDepthBiasSlope = 2.0f;
}

VulkanLocalShadowPass::VulkanLocalShadowPass(nvrhi::IDevice* nvrhiDevice, nvrhi::IBindingLayout* frameSetLayout, nvrhi::IBindingLayout* materialSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    const nvrhi::FormatSupport needed = nvrhi::FormatSupport::DepthStencil | nvrhi::FormatSupport::ShaderSample;
    nvrhi::TextureDesc desc;
    desc.width = kLocalShadowAtlasSize;
    desc.height = kLocalShadowAtlasSize;
    desc.format = (m_nvrhiDevice->queryFormatSupport(nvrhi::Format::D32) & needed) == needed ? nvrhi::Format::D32 : nvrhi::Format::D16;
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    desc.debugName = "Local shadow atlas";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    m_texture = m_nvrhiDevice->createTexture(desc);
    if (!m_texture)
    {
        throw std::runtime_error("Failed to create the local shadow atlas");
    }
    m_framebuffer = CreateNvrhiFramebuffer(m_nvrhiDevice, {}, m_texture);

    // As the cascades: a linear comparison sampler turns the shader's 3x3 taps into a 4x4 texel filter.
    // The shader keeps every tap inside its tile, so the address mode never matters; clamping keeps it
    // harmless anyway.
    nvrhi::SamplerDesc samplerDesc = BuildClampSamplerDesc(true);
    samplerDesc.reductionType = nvrhi::SamplerReductionType::Comparison;
    samplerDesc.comparisonFunc = nvrhi::ComparisonFunc::LessOrEqual;
    m_sampler = CreateNvrhiSampler(m_nvrhiDevice, samplerDesc, "Failed to create local shadow atlas sampler");

    ShadowCasterPipelineDesc casters;
    casters.depthFunc = nvrhi::ComparisonFunc::Less;
    casters.depthBias = kDepthBiasConstant;
    casters.slopeScaledDepthBias = kDepthBiasSlope;
    m_casters = std::make_unique<ShadowCasterRenderer>(m_nvrhiDevice, m_framebuffer->getFramebufferInfo(), frameSetLayout, materialSetLayout, casters);
    LOG_INFO("Created a {}x{} local shadow atlas of {} tiles", kLocalShadowAtlasSize, kLocalShadowAtlasSize, kLocalShadowTileCount);
}

VulkanLocalShadowPass::~VulkanLocalShadowPass() = default;

TextureDescriptorBinding VulkanLocalShadowPass::GetSampledBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_texture, m_sampler);
}

void VulkanLocalShadowPass::Record(
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameSet,
    std::span<const ShadowDrawItem> drawItems,
    std::span<const LocalShadowTile> tiles) const
{
    // Cleared whole every frame, and back where the material pass samples it.
    commandList->clearState();
    ClearDepth(commandList, m_texture, 1.0f);
    commandList->setTextureState(m_texture, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    commandList->commitBarriers();
    for (const LocalShadowTile& tile : tiles)
    {
        const nvrhi::ViewportState viewport = NativeViewportRect(
            tile.atlasOffsetTexels.x, tile.atlasOffsetTexels.y, kLocalShadowTileSize, kLocalShadowTileSize);
        m_casters->Record(
            commandList,
            m_framebuffer,
            viewport,
            frameSet,
            tile.viewProjection,
            drawItems,
            [&tile](const ShadowDrawItem& item)
            {
                return FrustumIntersectsSphere(tile.viewProjection, item.worldBoundsCenter, item.worldBoundsRadius);
            });
    }
    commandList->setTextureState(m_texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    commandList->clearState();
}
}
