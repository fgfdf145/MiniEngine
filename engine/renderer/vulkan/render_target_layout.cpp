#include "render_target_layout.h"

#include <algorithm>
#include <stdexcept>

namespace me
{

namespace
{
VkImageLayout ResolveReadLayout(RenderTargetId)
{
    return kReadLayout;
}

VkImageLayout ResolveWriteLayout(RenderTargetId target)
{
    return GetWriteLayout(target);
}
}

RenderTargetKind GetRenderTargetKind(RenderTargetId target)
{
    return target == RenderTargetId::SceneDepth ? RenderTargetKind::Depth : RenderTargetKind::Color;
}

VkImageLayout GetWriteLayout(RenderTargetId target)
{
    return GetRenderTargetKind(target) == RenderTargetKind::Depth
               ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

RenderTargetLayoutTracker::RenderTargetLayoutTracker()
{
    Reset();
}

void RenderTargetLayoutTracker::Reset()
{
    m_layouts.fill(VK_IMAGE_LAYOUT_UNDEFINED);
}

VkImageLayout RenderTargetLayoutTracker::GetLayout(RenderTargetId target) const
{
    return m_layouts.at(static_cast<size_t>(target));
}

std::vector<TargetTransition> RenderTargetLayoutTracker::Transition(const RenderPassIo& io)
{
    RequireDisjoint(io);

    std::vector<TargetTransition> transitions;
    transitions.reserve(io.reads.size() + io.writes.size());
    Accumulate(io.reads, &ResolveReadLayout, transitions);
    Accumulate(io.writes, &ResolveWriteLayout, transitions);
    return transitions;
}

void RenderTargetLayoutTracker::RequireDisjoint(const RenderPassIo& io) const
{
    for (RenderTargetId read : io.reads)
    {
        if (std::find(io.writes.begin(), io.writes.end(), read) != io.writes.end())
        {
            throw std::runtime_error("A render pass may not both read and write the same target");
        }
    }
}

void RenderTargetLayoutTracker::Accumulate(
    std::span<const RenderTargetId> targets,
    VkImageLayout (*resolve)(RenderTargetId),
    std::vector<TargetTransition>& transitions)
{
    for (const RenderTargetId target : targets)
    {
        const size_t index = static_cast<size_t>(target);
        const VkImageLayout required = resolve(target);
        if (m_layouts.at(index) == required)
        {
            continue;
        }

        transitions.push_back(TargetTransition{target, m_layouts.at(index), required});
        m_layouts.at(index) = required;
    }
}
}
