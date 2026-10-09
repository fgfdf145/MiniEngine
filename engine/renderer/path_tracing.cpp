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

uint32_t PathTraceMaxFrames(const PathTracingSettings& settings)
{
    return settings.offline.enabled ? kOfflinePathTraceMaxFrames : kPathTraceMaxFrames;
}

uint32_t PathTraceHistoryCap(const PathTracingSettings& settings, uint32_t stillFrames)
{
    const uint32_t limit = PathTraceMaxFrames(settings);
    const uint32_t maxFrames = static_cast<uint32_t>(std::clamp(settings.maxFrames, 1, static_cast<int>(limit)));
    const uint32_t motionFrames = std::min(static_cast<uint32_t>(std::max(settings.motionFrames, 1)), maxFrames);
    return std::min(motionFrames + std::min(stillFrames, limit), maxFrames);
}

uint32_t OfflineSamplesPerPixel(const OfflinePathTracingSettings& settings)
{
    return static_cast<uint32_t>(std::clamp(settings.samplesPerPixel, 1, 64));
}

uint32_t OfflineTargetFrames(const OfflinePathTracingSettings& settings)
{
    if (settings.targetSamples <= 0)
    {
        return 0;
    }
    const uint32_t samplesPerPixel = OfflineSamplesPerPixel(settings);
    const uint32_t frames = (static_cast<uint32_t>(settings.targetSamples) + samplesPerPixel - 1) / samplesPerPixel;
    return std::clamp(frames, 1u, kOfflinePathTraceMaxFrames);
}

PathTracingSettings EffectivePathTracing(const PathTracingSettings& settings)
{
    if (!settings.offline.enabled)
    {
        return settings;
    }
    PathTracingSettings effective = settings;
    const OfflinePathTracingSettings& offline = settings.offline;
    effective.maxBounces = std::clamp(offline.maxBounces, 1, 16);
    effective.lightCandidates = std::clamp(offline.lightCandidates, 1, 32);
    effective.fireflyClamp = std::max(offline.fireflyClamp, 0.0f);
    effective.accumulate = true;
    effective.motionFrames = 1;
    const uint32_t targetFrames = OfflineTargetFrames(offline);
    effective.maxFrames = static_cast<int>(targetFrames == 0 ? kOfflinePathTraceMaxFrames : targetFrames);
    effective.denoise = true;
    effective.forwardSurfaces = true;
    effective.forwardSurfacesHalfResolution = false;
    effective.rayMedia = true;
    effective.emissiveLights = true;
    effective.lightGrid = true;
    effective.reflectionGuides = true;
    effective.restir = false;
    return effective;
}

OfflineProgress OfflinePathTraceProgress(const OfflinePathTracingSettings& settings, uint32_t stillFrames)
{
    // A pixel averages one frame more each still frame, from the one it starts with.
    const uint32_t samplesPerPixel = OfflineSamplesPerPixel(settings);
    const uint32_t targetFrames = OfflineTargetFrames(settings);
    const uint32_t limit = targetFrames == 0 ? kOfflinePathTraceMaxFrames : targetFrames;
    OfflineProgress progress;
    progress.samples = std::min(stillFrames + 1u, limit) * samplesPerPixel;
    progress.targetSamples = targetFrames * samplesPerPixel;
    progress.done = targetFrames != 0 && stillFrames >= targetFrames;
    return progress;
}
}
