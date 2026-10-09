#include "pipeline_set.h"

#include "nvrhi_pass.h"

#include <engine/asset/mesh.h>
#include <engine/core/log/log.h>
#include <engine/renderer/material.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace me
{

nvrhi::InputLayoutHandle CreateMaterialInputLayout(nvrhi::IDevice* device, nvrhi::IShader* vertexShader)
{
    const auto attribute = [](const char* name, nvrhi::Format format, uint32_t buffer, uint32_t offset, uint32_t stride)
    {
        return nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setBufferIndex(buffer).setOffset(offset).setElementStride(stride);
    };
    // In location order: NVRHI's Vulkan backend numbers the attributes as listed.
    constexpr uint32_t kStride = sizeof(Vertex);
    const std::array<nvrhi::VertexAttributeDesc, 8> attributes = {
        attribute("POSITION", nvrhi::Format::RGB32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, position)), kStride),
        attribute("COLOR", nvrhi::Format::RGB32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, color)), kStride),
        attribute("TEXCOORD", nvrhi::Format::RG32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, texCoord)), kStride),
        attribute("NORMAL", nvrhi::Format::RGB32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, normal)), kStride),
        attribute("TANGENT", nvrhi::Format::RGBA32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, tangent)), kStride),
        attribute("SECOND_TEXCOORD", nvrhi::Format::RG32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, texCoord1)), kStride),
        attribute("OUTLINE_NORMAL", nvrhi::Format::RGB32_FLOAT, 0, static_cast<uint32_t>(offsetof(Vertex, outlineNormal)), kStride),
        attribute("PREVIOUS_POSITION", nvrhi::Format::RGB32_FLOAT, 1, 0, sizeof(float) * 3)};
    nvrhi::InputLayoutHandle layout = device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), vertexShader);
    if (!layout)
    {
        throw std::runtime_error("Failed to create the material vertex input layout");
    }
    return layout;
}

MaterialDrawConstants::MaterialDrawConstants(nvrhi::IDevice* device)
{
    const PushConstantLayout constants =
        CreatePushConstantLayout(device, 2, sizeof(ObjectPushConstants), nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel);
    layout = constants.layout;
    set = constants.set;
}

VulkanPipelineSet::VulkanPipelineSet(
    nvrhi::IDevice* device,
    const nvrhi::FramebufferInfo& framebuffer,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* materialSetLayout,
    nvrhi::IBindingLayout* drawConstantsLayout,
    const MaterialPipelineSetConfig& config)
{
    if (config.fragmentShader == nullptr || config.colorAttachmentCount == 0 || config.colorAttachmentCount > kMaxMaterialColorAttachments)
    {
        throw std::runtime_error("MaterialPipelineSetConfig needs a fragment shader and 1 to 8 color attachments");
    }
    const nvrhi::ShaderHandle vertexShader = CreateNvrhiShader(device, nvrhi::ShaderType::Vertex, "triangle.vert.spv");
    const nvrhi::ShaderHandle fragmentShader = CreateNvrhiShader(device, nvrhi::ShaderType::Pixel, config.fragmentShader);
    const nvrhi::InputLayoutHandle inputLayout = CreateMaterialInputLayout(device, vertexShader);

    for (MaterialAlphaMode mode : {MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask, MaterialAlphaMode::Blend})
    {
        for (bool doubleSided : {false, true})
        {
            const MaterialPipelineKey key{mode, doubleSided};
            const MaterialPipelineState state = GetMaterialPipelineState(key);
            const size_t index = GetMaterialPipelineIndex(key);

            // The fragment stage's specialization constants 0 (kAlphaMask), 1 (kScatterPrepass), 2
            // (kDecal) and 3 (kBlendItem).
            const std::array<nvrhi::ShaderSpecialization, 4> constants = {
                nvrhi::ShaderSpecialization::UInt32(0, state.alphaMaskEnabled ? 1u : 0u),
                nvrhi::ShaderSpecialization::UInt32(1, config.scatterPrepass ? 1u : 0u),
                nvrhi::ShaderSpecialization::UInt32(2, config.decal ? 1u : 0u),
                nvrhi::ShaderSpecialization::UInt32(3, mode == MaterialAlphaMode::Blend ? 1u : 0u)};

            nvrhi::GraphicsPipelineDesc desc;
            desc.VS = vertexShader;
            desc.PS = SpecializeShader(device, fragmentShader, constants, config.fragmentShader);
            desc.inputLayout = inputLayout;
            desc.primType = nvrhi::PrimitiveType::TriangleList;
            desc.bindingLayouts = {frameSetLayout, materialSetLayout, drawConstantsLayout};

            // Winding: Vulkan framebuffer Y points down, which alone would flip glTF's CCW front faces
            // to CW, but the render projection's Y-flip (proj[1][1] *= -1, see UpdateViewportMatrices)
            // flips them back, so front faces arrive counter-clockwise in framebuffer space. Materials
            // flagged doubleSided (glTF doubleSided=true, e.g. foliage/glass) use the no-cull variant.
            nvrhi::RasterState& raster = desc.renderState.rasterState;
            raster.setFillSolid().setFrontCounterClockwise(true);
            raster.setCullMode(state.cullBackFaces ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);

            // The layer's surface pass has no depth attachment: the fragments it keeps are the ones at
            // the depth its first pass found.
            nvrhi::DepthStencilState& depth = desc.renderState.depthStencilState;
            depth.setDepthTestEnable(config.layerPass != 2);
            depth.setDepthWriteEnable(state.depthWriteEnabled && !config.decal && config.layerPass == 0);
            depth.setDepthFunc(config.depthLessOrEqual ? nvrhi::ComparisonFunc::GreaterOrEqual : nvrhi::ComparisonFunc::Greater);
            depth.setStencilEnable(false);

            // Whether alpha is written at all is the config's call: the forward set keeps the HDR
            // target's clear alpha, the geometry set writes every G-buffer channel.
            const nvrhi::ColorMask writeMask = config.writeAlpha ? nvrhi::ColorMask::All : nvrhi::ColorMask(nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue);
            for (uint32_t attachment = 0; attachment < config.colorAttachmentCount; ++attachment)
            {
                nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[attachment];
                blend.setBlendEnable(state.blendEnabled && config.allowBlending)
                    .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                    .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                    .setBlendOp(nvrhi::BlendOp::Add)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                    .setBlendOpAlpha(nvrhi::BlendOp::Add)
                    .setColorWriteMask(writeMask);
                if (config.decal)
                {
                    // Geometry pass order (VulkanGeometryPass::kAttachments): albedo, normal, surface,
                    // emissive, then the rest, which a decal leaves as the surface under it wrote them.
                    const nvrhi::ColorMask kRgb = nvrhi::ColorMask(nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue);
                    const std::array<nvrhi::ColorMask, 4> decalMasks = {
                        kRgb, nvrhi::ColorMask(0), nvrhi::ColorMask(nvrhi::ColorMask::Red | nvrhi::ColorMask::Green), kRgb};
                    blend.setColorWriteMask(attachment < decalMasks.size() ? decalMasks[attachment] : nvrhi::ColorMask(0));
                    blend.setBlendEnable(blend.colorWriteMask != nvrhi::ColorMask(0));
                }
                if (config.layerPass == 1)
                {
                    // The nearest fragment's depth (reverse-Z: the greatest).
                    blend.setBlendEnable(true)
                        .setSrcBlend(nvrhi::BlendFactor::One)
                        .setDestBlend(nvrhi::BlendFactor::One)
                        .setBlendOp(nvrhi::BlendOp::Max)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::One)
                        .setBlendOpAlpha(nvrhi::BlendOp::Max)
                        .setColorWriteMask(nvrhi::ColorMask::Red);
                }
            }

            m_pipelines[index] = device->createGraphicsPipeline(desc, framebuffer);
            if (!m_pipelines[index])
            {
                throw std::runtime_error(std::string("Failed to create a material pipeline for ") + config.fragmentShader);
            }
        }
    }
    LOG_INFO("Created {} material pipeline variants for {}", m_pipelines.size(), config.fragmentShader);
}

VulkanPipelineSet::~VulkanPipelineSet() = default;

nvrhi::IGraphicsPipeline* VulkanPipelineSet::Get(MaterialPipelineKey key) const
{
    return m_pipelines.at(GetMaterialPipelineIndex(key));
}
}
