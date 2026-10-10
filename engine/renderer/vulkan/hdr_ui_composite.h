#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace me
{

// The editor frame on an HDR10 swapchain (docs/design/2026-10-10-hdr-calibration-design.md). ImGui
// draws into a float layer the swapchain's size, sRGB-encoded as the SDR swapchain holds it, so
// translucent UI blends exactly as it does in SDR (blending PQ code values instead made every
// translucent fill darker); hdr_ui_encode.frag then writes the layer to the swapchain in PQ, its 1.0
// at the UI white.
class HdrUiComposite
{
  public:
    HdrUiComposite(nvrhi::IDevice* device, uint32_t width, uint32_t height);

    HdrUiComposite(const HdrUiComposite&) = delete;
    HdrUiComposite& operator=(const HdrUiComposite&) = delete;

    // What ImGui draws into, cleared to clearColor (an sRGB-encoded colour) and left as a render target.
    nvrhi::IFramebuffer* BeginLayer(nvrhi::ICommandList* commandList, const nvrhi::Color& clearColor);
    // The layer into backBuffer (one of the swapchain's framebuffers), PQ-encoded with 1.0 at
    // uiWhiteNits and lifted onto the black floor blackFloorPq (HdrBlackFloorPq; 0 for none).
    void Encode(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* backBuffer, float uiWhiteNits, float blackFloorPq);

  private:
    nvrhi::IDevice* m_device = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    nvrhi::TextureHandle m_layer;
    nvrhi::FramebufferHandle m_layerFramebuffer;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    // Made with the first back buffer it encodes into (the swapchain's format).
    nvrhi::GraphicsPipelineHandle m_pipeline;
};
}
