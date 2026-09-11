#pragma once

#include "common.h"
#include "render_target_layout.h"

#include <imgui.h>

#include <array>
#include <vector>

namespace me
{

// Owns every offscreen image the scene passes use, their views and their memory, plus the ImGui
// texture bindings needed to display them. Render passes and framebuffers deliberately live in
// the passes that use them, not here: a pass knows its own attachment set, and keeping them apart
// is what lets a resize rebuild images without touching a render pass the pipelines were built
// against.
//
// Two indexing schemes coexist, which is deliberate and is why the accessors are separate:
//
//   * SceneDepth and SceneHdr are transient. They are written and read inside one command buffer,
//     so kMaxFramesInFlight copies suffice, indexed by VulkanCommandContext::GetCurrentFrame().
//   * SceneLdr is sampled by ImGui, whose texture binding is handed out before the command buffer
//     is recorded, so it keeps one copy per swapchain image, indexed by the acquired image index.
//
// Passing a frame slot where an image index belongs would silently sample the wrong target. What
// prevents that is ResolveIndex: every caller holding both an index and a slot routes through it
// rather than picking one itself. GetImage / GetView only call .at() on the target's own vector,
// which catches an out-of-range index and nothing more — with two transient copies against
// typically three swapchain images, the confusion that matters is in range and would pass
// silently.
class SceneRenderTargets
{
  public:
    SceneRenderTargets(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkFormat ldrFormat,
        VkExtent2D extent,
        uint32_t swapchainImageCount);
    ~SceneRenderTargets();

    SceneRenderTargets(const SceneRenderTargets&) = delete;
    SceneRenderTargets& operator=(const SceneRenderTargets&) = delete;

    VkFormat GetFormat(RenderTargetId target) const;
    VkImageAspectFlags GetAspect(RenderTargetId target) const;
    VkImage GetImage(RenderTargetId target, uint32_t index) const;
    VkImageView GetView(RenderTargetId target, uint32_t index) const;

    VkExtent2D GetExtent() const;
    bool MatchesExtent(VkExtent2D extent) const;

    // Copy counts for the two indexing schemes. Transient covers SceneDepth and SceneHdr.
    uint32_t GetTransientCopyCount() const;
    uint32_t GetLdrCopyCount() const;

    // Picks the index appropriate to a target's scheme. Every caller that has both an image index
    // and a frame slot in hand goes through this instead of restating the rule, so the rule lives
    // in exactly one place — the class that decided it.
    uint32_t ResolveIndex(RenderTargetId target, uint32_t imageIndex, uint32_t frameSlot) const;

    ImTextureID GetLdrTextureId(uint32_t imageIndex) const;

    // Both must run with the in-flight frames already waited on, and ReleaseImages must run while
    // ImGui's Vulkan backend is still alive because it removes ImGui texture bindings.
    //
    // Rebuild re-creates the images at a new extent and swapchain image count; it does not re-run
    // format selection, so a caller whose swapchain format may have moved must construct a new
    // instance instead (see VulkanRenderer::CreateSwapchainResources).
    void ReleaseImages();
    void Rebuild(VkExtent2D extent, uint32_t swapchainImageCount);

  private:
    struct TargetImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet imguiBinding = VK_NULL_HANDLE;
    };

    struct TargetDescription
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        bool bindToImGui = false;
        std::vector<TargetImage> images;
    };

    VkFormatFeatureFlags QueryFormatFeatures(VkFormat format) const;
    void SelectFormats(VkFormat ldrFormat);
    void CreateSampler();
    void CreateImages(uint32_t swapchainImageCount);
    void DestroyImages(std::array<TargetDescription, kRenderTargetCount>& targets) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void CreateImage(VkFormat format, VkImageUsageFlags usage, TargetImage& target) const;
    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const;
    TargetDescription& Describe(RenderTargetId target);
    const TargetDescription& Describe(RenderTargetId target) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    VkSampler m_sampler = VK_NULL_HANDLE;
    uint32_t m_swapchainImageCount = 0;
    std::array<TargetDescription, kRenderTargetCount> m_targets{};
};
}
