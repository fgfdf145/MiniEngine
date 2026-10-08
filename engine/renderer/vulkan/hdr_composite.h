#pragma once

#include "common.h"

#include <engine/editor/imgui_frame_snapshot.h>

#include <memory>
#include <optional>
#include <vector>

struct ImDrawData;

namespace me
{

class VulkanRenderPass;

// HDR output with an SDR editor (docs/design/2026-10-08-viewport-only-hdr-design.md). ImGui draws the
// editor into an SDR layer exactly as it draws the SDR swapchain, except that the viewport image's
// rectangle is cut out of it (the draw commands that would sample the image clear it instead); a
// full-screen pass then puts that layer over the HDR scene image and writes the HDR swapchain. Only the
// viewport is HDR; every UI pixel is the SDR frame's, shown at the SDR content brightness.
class VulkanHdrComposite
{
  public:
    // The UI layer's format: the SDR swapchain's.
    static constexpr VkFormat kUiFormat = VK_FORMAT_B8G8R8A8_UNORM;

    // compositeRenderPass is the swapchain's pass (VulkanRenderPass on the swapchain images). One UI
    // layer image per swapchain image, as every other per-frame image here.
    VulkanHdrComposite(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkExtent2D extent,
        uint32_t imageCount,
        VkRenderPass compositeRenderPass);
    ~VulkanHdrComposite();

    VulkanHdrComposite(const VulkanHdrComposite&) = delete;
    VulkanHdrComposite& operator=(const VulkanHdrComposite&) = delete;

    // The pass ImGui draws the UI layer with; ImGui's pipeline is built against it.
    VkRenderPass GetUiRenderPass() const;
    VkFramebuffer GetUiFramebuffer(uint32_t imageIndex) const;

    // Before the frame's ImGui draw: turns the commands that sample viewportTexture into cuts in the
    // layer, followed by resetRenderState (ImGui's platform DrawCallback_ResetRenderState). Remembers
    // where the image was for RecordComposite.
    void PrepareDrawData(ImGuiFrameSnapshot& snapshot, ImTextureID viewportTexture, ImDrawCallback resetRenderState);
    // ImGui's draw of the UI layer is recorded into this command buffer (the cut needs it).
    void BeginUiRecording(VkCommandBuffer commandBuffer, const ImDrawData& drawData);

    // The composite, inside the swapchain pass whose framebuffer is bound: the UI layer of imageIndex
    // (left shader-read by its pass) over sceneView (shader-read), encoded for the swapchain.
    // sdrWhiteNits is where SDR white shows: Windows' SDR content brightness.
    void RecordComposite(
        VkCommandBuffer commandBuffer,
        uint32_t imageIndex,
        VkImageView sceneView,
        bool scRgb,
        float sdrWhiteNits);

  private:
    struct LayerImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    static void CutViewport(const ImDrawList* list, const ImDrawCmd* command);
    void CreateLayerImages(VkPhysicalDevice physicalDevice, uint32_t imageCount);
    void CreatePipelines(VkPipelineCache pipelineCache, VkRenderPass compositeRenderPass);
    void Destroy();

    VkDevice m_device = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    std::vector<LayerImage> m_layers;
    std::unique_ptr<VulkanRenderPass> m_uiRenderPass;
    VkPipelineLayout m_cutLayout = VK_NULL_HANDLE;
    VkPipeline m_cutPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkPipelineLayout m_compositeLayout = VK_NULL_HANDLE;
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;

    // This frame's: where the viewport image was, and what the cut callback records into.
    std::optional<ImGuiCommandQuad> m_viewportQuad;
    VkCommandBuffer m_recording = VK_NULL_HANDLE;
    ImVec2 m_displayPos{0.0f, 0.0f};
    ImVec2 m_framebufferScale{1.0f, 1.0f};
};
}
