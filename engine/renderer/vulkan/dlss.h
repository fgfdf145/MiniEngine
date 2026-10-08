#pragma once

#include "common.h"

#include <engine/renderer/render_types.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace me
{

// One image NGX reads or writes: the image, the view it is given, the view's format and the size of
// the region it uses.
struct DlssImage
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkExtent2D extent{};
};

// What one DLSS evaluation reads and writes. color, depth and motionVectors are at the render size
// and in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; output is at the output size and in
// VK_IMAGE_LAYOUT_GENERAL. NGX leaves them in those layouts.
struct DlssEvaluateInputs
{
    DlssImage color;
    DlssImage depth;
    // RG16F or RG32F, render pixels, pointing from this frame's position to last frame's.
    DlssImage motionVectors;
    DlssImage output;
    // This frame's projection jitter in render pixels, x right and y down: the shift the jitter gave
    // the image (JitterProjection).
    glm::vec2 jitterPixels{0.0f};
    // Throws the history away: the first frame after a cut, a resize or a mode change.
    bool reset = false;
    float frameTimeMs = 0.0f;

    // Ray reconstruction only (a feature made with rayReconstruction): the guides it denoises with, at
    // the render size and in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (dlss_rr_guides.comp): the
    // diffuse and specular albedos and the world space normal with the roughness in w; and the camera,
    // world to view and the unjittered view to clip, glm's (column-major) matrices.
    DlssImage diffuseAlbedo;
    DlssImage specularAlbedo;
    DlssImage normalRoughness;
    // Optional (null images: none): the specular ray's hit distance, world units (R16F), and the
    // motion vectors of what the reflections show (RG16F, as motionVectors), which the path tracer's
    // raw paths come with (dlss_rr_guides.comp).
    DlssImage specularHitDistance;
    DlssImage reflectionMotionVectors;
    glm::mat4 worldToView{1.0f};
    glm::mat4 viewToClip{1.0f};
};

// NVIDIA DLSS super resolution and DLAA through the NGX SDK (docs/design/2026-10-07-dlss-design.md),
// and with them DLSS ray reconstruction (DLSS-D), which denoises the ray traced effects as it upscales
// (docs/design/2026-10-07-ray-traced-effects-design.md).
// Built only with the SDK (MINIENGINE_WITH_DLSS); without it, or on a device or driver without DLSS,
// IsAvailable() is false and the renderer keeps its own TAA. Every call is the render thread's.
class VulkanDlss
{
  public:
    // The instance and device extensions NGX needs, to enable when the instance and the device are
    // made; empty without the SDK.
    static std::vector<std::string> RequiredInstanceExtensions();
    static std::vector<std::string> RequiredDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice);

    // Starts NGX on this device. extensionsEnabled: every extension the two lists above named was
    // enabled; otherwise DLSS stays unavailable. dataDirectory is where NGX writes its logs.
    VulkanDlss(
        VkInstance instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue,
        bool extensionsEnabled);
    ~VulkanDlss();

    VulkanDlss(const VulkanDlss&) = delete;
    VulkanDlss& operator=(const VulkanDlss&) = delete;

    bool IsAvailable() const;
    // Ray reconstruction runs here too (the driver has it and its runtime was found).
    bool IsRayReconstructionAvailable() const;
    // Why DLSS is unavailable, or the DLSS version that runs; for the editor.
    const std::string& Status() const;

    // The size to render at for this output size and mode (DLSS's optimal settings), the output size
    // itself for DLAA. Empty when unavailable or the mode is off.
    std::optional<VkExtent2D> RenderExtentFor(VkExtent2D output, DlssMode mode);

    // Makes the DLSS feature for these sizes, mode and preset, unless the current one already is;
    // waits for the GPU. rayReconstruction makes the ray reconstruction feature instead of super
    // resolution, where it is available (the preset applies to super resolution). False, with Status()
    // saying why, when it cannot.
    bool EnsureFeature(VkExtent2D render, VkExtent2D output, DlssMode mode, DlssPreset preset, bool rayReconstruction = false);
    void ReleaseFeature();
    bool HasFeature() const;
    // The feature is ray reconstruction: Evaluate reads the guides.
    bool HasRayReconstruction() const;

    // Records the evaluation into commandBuffer. False when there is no feature or NGX refused.
    bool Evaluate(VkCommandBuffer commandBuffer, const DlssEvaluateInputs& inputs);

  private:
    struct Ngx;
    std::unique_ptr<Ngx> m_ngx;
    bool m_available = false;
    bool m_rayReconstructionAvailable = false;
    std::string m_status;
};
}
