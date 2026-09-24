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
    switch (target)
    {
    case RenderTargetId::SceneDepth:
        return RenderTargetKind::Depth;
    case RenderTargetId::AoRaw:
    case RenderTargetId::SceneAo:
    case RenderTargetId::SceneTaa:
        return RenderTargetKind::Storage;
    default:
        return RenderTargetKind::Color;
    }
}

VkImageLayout GetWriteLayout(RenderTargetId target)
{
    switch (GetRenderTargetKind(target))
    {
    case RenderTargetKind::Depth:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case RenderTargetKind::Storage:
        return VK_IMAGE_LAYOUT_GENERAL;
    default:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
}

RenderTargetLayoutTracker::RenderTargetLayoutTracker()
{
    Reset();
}

void RenderTargetLayoutTracker::Reset()
{
    m_layouts.fill(VK_IMAGE_LAYOUT_UNDEFINED);
    m_lastAccessWasWrite.fill(false);
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
    Accumulate(io.reads, &ResolveReadLayout, false, transitions);
    Accumulate(io.writes, &ResolveWriteLayout, true, transitions);
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
    bool isWrite,
    std::vector<TargetTransition>& transitions)
{
    for (const RenderTargetId target : targets)
    {
        const size_t index = static_cast<size_t>(target);
        const VkImageLayout required = resolve(target);
        const bool writeAfterWrite = isWrite && m_lastAccessWasWrite.at(index);
        m_lastAccessWasWrite.at(index) = isWrite;
        if (m_layouts.at(index) == required && !writeAfterWrite)
        {
            continue;
        }

        // When the layout already matches, this is the memory-only barrier described in the
        // header: oldLayout == newLayout.
        transitions.push_back(TargetTransition{target, m_layouts.at(index), required});
        m_layouts.at(index) = required;
    }
}
}
