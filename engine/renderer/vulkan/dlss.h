#pragma once

#include "common.h"

#include <engine/renderer/render_types.h>

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace me
{

// One image NGX reads or writes: the image, the view it is given, the view's format and the size of
// the region it uses. On Direct3D 12 NGX takes the resource alone (texture); image and view are
// Vulkan's.
struct DlssImage
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkExtent2D extent{};
    nvrhi::ITexture* texture = nullptr;

    bool IsSet() const
    {
        return image != VK_NULL_HANDLE || texture != nullptr;
    }
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

// The DLSS features NGX keeps at once, one per view that resolves with DLSS: the viewport's, and
// Photo Mode's view, whose size, mode and model are its own.
enum class DlssFeatureSlot : uint32_t
{
    Viewport = 0,
    Photo = 1
};
inline constexpr uint32_t kDlssFeatureSlotCount = 2;

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
    // Starts NGX's Direct3D 12 API on device's ID3D12Device; features are made on command lists of
    // device's own.
    explicit VulkanDlss(nvrhi::IDevice* d3d12Device);
    ~VulkanDlss();

    VulkanDlss(const VulkanDlss&) = delete;
    VulkanDlss& operator=(const VulkanDlss&) = delete;

    bool IsAvailable() const;
    // Ray reconstruction runs here too (the driver has it and its runtime was found).
    bool IsRayReconstructionAvailable() const;
    // Why DLSS is unavailable, or the DLSS version that runs; for the editor.
    const std::string& Status() const;

    // The size to render at for this output size and mode (DLSS's optimal settings), the output size
    // itself for DLAA. Empty when unavailable or the mode is off. Each slot remembers its last answer.
    std::optional<VkExtent2D> RenderExtentFor(VkExtent2D output, DlssMode mode, DlssFeatureSlot slot = DlssFeatureSlot::Viewport);

    // Makes the slot's DLSS feature for these sizes, mode and preset, unless it already is; waits for
    // the GPU. rayReconstruction makes the ray reconstruction feature instead of super resolution,
    // where it is available (the preset applies to super resolution). False, with Status() saying why,
    // when it cannot. The slots' features are independent: the photo's never disturbs the viewport's.
    bool EnsureFeature(
        VkExtent2D render,
        VkExtent2D output,
        DlssMode mode,
        DlssPreset preset,
        bool rayReconstruction = false,
        DlssFeatureSlot slot = DlssFeatureSlot::Viewport);
    void ReleaseFeature(DlssFeatureSlot slot = DlssFeatureSlot::Viewport);
    bool HasFeature(DlssFeatureSlot slot = DlssFeatureSlot::Viewport) const;
    // The feature is ray reconstruction: Evaluate reads the guides.
    bool HasRayReconstruction(DlssFeatureSlot slot = DlssFeatureSlot::Viewport) const;

    // Records the slot's evaluation into the command list's native command buffer or list. False when
    // it has no feature or NGX refused. NGX binds what it likes: clear the list's state after.
    bool Evaluate(nvrhi::ICommandList* commandList, const DlssEvaluateInputs& inputs, DlssFeatureSlot slot = DlssFeatureSlot::Viewport);

  private:
    // The NGX capabilities read once NGX started (either API): sets m_available and the status.
    void ReadCapabilities();

    struct Ngx;
    std::unique_ptr<Ngx> m_ngx;
    bool m_available = false;
    bool m_rayReconstructionAvailable = false;
    std::string m_status;
};
}
