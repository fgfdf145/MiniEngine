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

// Every offscreen target the scene passes read or write. Phase one uses all three; the G-buffer
// ids are appended here when the deferred passes land, which is why Count is last.
enum class RenderTargetId : uint32_t
{
    SceneDepth,
    SceneHdr,
    SceneLdr,
    Count
};

inline constexpr size_t kRenderTargetCount = static_cast<size_t>(RenderTargetId::Count);

// A depth target transitions to a different attachment layout than a color target, and the
// tracker has to know which is which without ever touching a VkImage.
enum class RenderTargetKind
{
    Color,
    Depth
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
        std::vector<TargetTransition>& transitions);

    std::array<VkImageLayout, kRenderTargetCount> m_layouts{};
};
}
