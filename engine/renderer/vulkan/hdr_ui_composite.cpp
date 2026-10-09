#include "hdr_ui_composite.h"

#include "nvrhi_pass.h"

#include <stdexcept>

namespace me
{

namespace
{
// hdr_ui_encode.frag's block.
struct HdrUiEncodeConstants
{
    float uiWhiteNits = 203.0f;
    float padding[3] = {};
};

static_assert(sizeof(HdrUiEncodeConstants) == 16, "HdrUiEncodeConstants must match the shader's block");
}

HdrUiComposite::HdrUiComposite(nvrhi::IDevice* device, uint32_t width, uint32_t height)
    : m_device(device)
    , m_width(width)
    , m_height(height)
{
    // fp16: the UI's 8-bit steps survive exactly, and the scene's highlights (up to the peak over the
    // UI white, sRGB-encoded past 1) keep their level.
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    desc.debugName = "HDR UI layer";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    m_layer = m_device->createTexture(desc);
    if (!m_layer)
    {
        throw std::runtime_error("Failed to create the HDR UI layer");
    }
    m_layerFramebuffer = CreateNvrhiFramebuffer(m_device, {m_layer.Get()});

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = ShaderBindingOffsets();
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(HdrUiEncodeConstants))};
    m_bindingLayout = CreateNvrhiBindingLayout(m_device, layoutDesc, "Failed to create the HDR UI encode binding layout");
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(0, m_layer),
        nvrhi::BindingSetItem::PushConstants(0, sizeof(HdrUiEncodeConstants))};
    m_bindingSet = CreateNvrhiBindingSet(m_device, setDesc, m_bindingLayout, "Failed to create the HDR UI encode binding set");
}

nvrhi::IFramebuffer* HdrUiComposite::BeginLayer(nvrhi::ICommandList* commandList, const nvrhi::Color& clearColor)
{
    ClearTextureFloat(commandList, m_layer, clearColor);
    commandList->setTextureState(m_layer, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->commitBarriers();
    return m_layerFramebuffer;
}

void HdrUiComposite::Encode(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* backBuffer, float uiWhiteNits)
{
    if (!m_pipeline)
    {
        m_pipeline = CreateFullscreenNvrhiPipeline(m_device, backBuffer->getFramebufferInfo(), {m_bindingLayout.Get()}, "hdr_ui_encode.frag.spv");
    }
    commandList->setTextureState(m_layer, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = backBuffer;
    state.viewport = NativeViewportState(VkExtent2D{m_width, m_height});
    state.bindings = {m_bindingSet};
    commandList->setGraphicsState(state);
    HdrUiEncodeConstants constants;
    constants.uiWhiteNits = uiWhiteNits;
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}
}
