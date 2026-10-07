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
// more for every still frame, up to maxFrames. Both are clamped to what the history's half-float
// sample count holds exactly.
uint32_t PathTraceHistoryCap(const PathTracingSettings& settings, uint32_t stillFrames);

// kPathTraceMaxFrames: 2048, the largest count a half float increments exactly.
inline constexpr uint32_t kPathTraceMaxFrames = 2048;
}
