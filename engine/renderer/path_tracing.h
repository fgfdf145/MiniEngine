#pragma once

#include <engine/renderer/render_types.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// What a path traced frame is seen from: the view, the unjittered projection and the render size.
struct PathTraceView
{
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    uint32_t width = 0;
    uint32_t height = 0;
};

// Whether the path tracer's image is still converging toward the same picture
// (docs/design/2026-10-07-path-tracing-design.md): a still camera over a still scene under the same
// light and settings averages every frame it traces, up to PathTracingSettings::maxFrames, which is
// the reference; anything that changes what a pixel sees limits the average to motionFrames again.
// Exact comparisons, unlike the DDGI lighting watch's tolerance: a sun the time of day moves a little
// each frame is not a still image.
class PathTraceAccumulation
{
  public:
    // Called once per path traced frame, with the lighting as values that describe it (every selected
    // light, the sky's) and whether the scene changed (an instance moved, content was installed). The
    // frames everything has stood still before this one: 0 when anything changed, or on the first
    // frame.
    uint32_t Advance(const PathTraceView& view, std::span<const glm::vec4> lighting, const RenderDebugSettings& settings, bool sceneChanged);

    // The next Advance starts over: a frame that did not path trace, or rebuilt history images.
    void Reset();

  private:
    bool m_hasPrevious = false;
    PathTraceView m_view;
    std::vector<glm::vec4> m_lighting;
    RenderDebugSettings m_settings;
    uint32_t m_stillFrames = 0;
};

// The longest history a pixel may average this frame: motionFrames while anything changes, then one
// more for every still frame, up to maxFrames. Both are clamped to what the history's sample count
// holds exactly: a half float's, or the offline mode's full float (PathTraceMaxFrames).
uint32_t PathTraceHistoryCap(const PathTracingSettings& settings, uint32_t stillFrames);

// kPathTraceMaxFrames: 2048, the largest count a half float increments exactly.
inline constexpr uint32_t kPathTraceMaxFrames = 2048;
// The offline mode's histories are full floats (exact to 2^24); a frame's sample then adds at least
// 1/65536 of itself, well above their rounding.
inline constexpr uint32_t kOfflinePathTraceMaxFrames = 65536;

// The most frames a history of these settings averages: kOfflinePathTraceMaxFrames in the offline
// mode, kPathTraceMaxFrames otherwise.
uint32_t PathTraceMaxFrames(const PathTracingSettings& settings);

// The settings the renderer runs (docs/design/2026-10-09-path-tracing-offline-mode-design.md): as they
// are, or in the offline mode every path tracing feature on (ReSTIR PT off: the offline image is the
// plain path tracer's unbiased accumulation), the offline bounces, light candidates and firefly clamp,
// no accumulation while anything moves (its correlated noise is what ray reconstruction keeps as
// texture), and as many still frames as the target's samples take.
PathTracingSettings EffectivePathTracing(const PathTracingSettings& settings);

// The offline mode's samples a pixel each frame, and the still frames that reach its target (0: no
// target, never done).
uint32_t OfflineSamplesPerPixel(const OfflinePathTracingSettings& settings);
uint32_t OfflineTargetFrames(const OfflinePathTracingSettings& settings);

// Where an offline image stands after this many still frames: the samples each pixel has (the frames
// it averages, at most the target's, times the samples a frame), and whether the target is reached
// and the trace stops (the frame after the last one it needed).
struct OfflineProgress
{
    uint32_t samples = 0;
    uint32_t targetSamples = 0;
    bool done = false;
};
OfflineProgress OfflinePathTraceProgress(const OfflinePathTracingSettings& settings, uint32_t stillFrames);
}
