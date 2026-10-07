#include "path_tracing.h"

#include <algorithm>

namespace me
{

uint32_t PathTraceAccumulation::Advance(
    const PathTraceView& view,
    std::span<const glm::vec4> lighting,
    const RenderDebugSettings& settings,
    bool sceneChanged)
{
    const bool still = m_hasPrevious && !sceneChanged && view.view == m_view.view && view.projection == m_view.projection &&
                       view.width == m_view.width && view.height == m_view.height && settings == m_settings &&
                       std::equal(lighting.begin(), lighting.end(), m_lighting.begin(), m_lighting.end());
    m_stillFrames = still ? m_stillFrames + 1u : 0u;
    m_hasPrevious = true;
    m_view = view;
    m_lighting.assign(lighting.begin(), lighting.end());
    m_settings = settings;
    return m_stillFrames;
}

void PathTraceAccumulation::Reset()
{
    m_hasPrevious = false;
    m_stillFrames = 0;
}

uint32_t PathTraceHistoryCap(const PathTracingSettings& settings, uint32_t stillFrames)
{
    const uint32_t maxFrames = static_cast<uint32_t>(std::clamp(settings.maxFrames, 1, static_cast<int>(kPathTraceMaxFrames)));
    const uint32_t motionFrames = std::min(static_cast<uint32_t>(std::max(settings.motionFrames, 1)), maxFrames);
    return std::min(motionFrames + std::min(stillFrames, kPathTraceMaxFrames), maxFrames);
}
}
