#pragma once

// Deliberately not including "common.h": this header is compiled into a unit test that must not
// pull in SDL, GLM or the editor. Vulkan's own header supplies every type used below.
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// Every offscreen target the scene passes read or write. The first three arrived in phase one,
// the four G-buffer targets in phase two, then GBufferVelocity (motion vectors) and the two AO
// targets; GB4 (entity id) is appended in phase three, which is why Count stays last. Only SceneLdr is indexed by swapchain
// image; every other target is transient and indexed by frame slot (see
// SceneRenderTargets::ResolveIndex).
enum class RenderTargetId : uint32_t
{
    SceneDepth,
    SceneHdr,
    SceneLdr,
    GBufferAlbedo,
    GBufferNormal,
    GBufferSurface,
    GBufferEmissive,
    GBufferVelocity,
    // GB5: data whose meaning depends on the pixel's shading model (GB2.a). Clearcoat keeps its
    // factor and roughness in .rg.
    GBufferCustom,
    // Visibility bitmask AO: the noisy trace, then the filtered result the lighting pass reads.
    // Both are storage images written by compute.
    AoRaw,
    SceneAo,
    Count
};

inline constexpr size_t kRenderTargetCount = static_cast<size_t>(RenderTargetId::Count);

// A depth target transitions to a different attachment layout than a color target, and the
// tracker has to know which is which without ever touching a VkImage.
enum class RenderTargetKind
{
    Color,
    Depth,
    // Written by a compute shader through image stores, in VK_IMAGE_LAYOUT_GENERAL.
    Storage
};

RenderTargetKind GetRenderTargetKind(RenderTargetId target);

// The layout a target must be in to be written by a pass, derived from its kind.
VkImageLayout GetWriteLayout(RenderTargetId target);

// The layout a target must be in to be sampled by a pass.
inline constexpr VkImageLayout kReadLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// What a pass declares about the targets it touches. Reads resolve to kReadLayout, writes to
// GetWriteLayout. A target may not appear in both spans.
struct RenderPassIo
{
    std::span<const RenderTargetId> reads;
    std::span<const RenderTargetId> writes;
};

struct TargetTransition
{
    RenderTargetId target = RenderTargetId::SceneDepth;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool operator==(const TargetTransition&) const = default;
};

// Tracks the layout every target is currently in and works out the barriers a pass needs.
//
// A barrier is also needed when nothing changes layout: a target written by one pass and written
// again by the next (depth by the geometry pass and then the forward pass, HDR by the lighting pass
// and then the forward blend pass) must have the second write ordered after the first. Transition
// reports that as a TargetTransition whose oldLayout equals its newLayout, a memory-only barrier.
// A read after a read needs nothing and produces nothing.
//
// This class calls no Vulkan entry point. Transition records the new layouts and returns the
// barriers the caller must issue; recording and bookkeeping are one call so a caller cannot
// update the tracker without issuing the barriers or the reverse.
//
// It is authoritative only because every render pass in the frame declares
// initialLayout == finalLayout for its attachments. A render pass that transitions an attachment
// implicitly would desynchronise this tracker silently.
//
// It is also frame-scoped by construction, which is the invariant a target author can break: the
// tracker holds one layout per target, while a transient target holds one image per copy, so a
// layout carried across frames would describe a different VkImage than the one being touched.
// Reset therefore runs at the head of every command buffer. The consequence is that a target whose
// contents must survive across frames cannot use this tracker unchanged; it would need its layout
// tracked per copy rather than per target.
//
// This header stays ASCII: it is compiled into a unit test target that does not pass /utf-8.
class RenderTargetLayoutTracker
{
  public:
    RenderTargetLayoutTracker();

    // Returns every target to VK_IMAGE_LAYOUT_UNDEFINED. Two call sites: the head of every command
    // buffer, which is the one that makes the tracker frame-scoped and therefore correct against
    // per-copy images; and whenever the images themselves are recreated, since a fresh VkImage is
    // undefined regardless of what the destroyed one was in.
    void Reset();

    VkImageLayout GetLayout(RenderTargetId target) const;

    // Throws std::runtime_error when a target appears in both spans of one declaration.
    std::vector<TargetTransition> Transition(const RenderPassIo& io);

  private:
    void RequireDisjoint(const RenderPassIo& io) const;
    void Accumulate(
        std::span<const RenderTargetId> targets,
        VkImageLayout (*resolve)(RenderTargetId),
        bool isWrite,
        std::vector<TargetTransition>& transitions);

    std::array<VkImageLayout, kRenderTargetCount> m_layouts{};
    // Whether the last pass to touch each target wrote it, so a following write can be ordered
    // after it even though the layout stays the same.
    std::array<bool, kRenderTargetCount> m_lastAccessWasWrite{};
};
}
