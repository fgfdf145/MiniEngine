#pragma once

#include "camera.h"
#include "render_types.h"

namespace me
{

// A camera the scene is drawn from in a frame besides the viewport's: one of a quad recording's
// (docs/design/2026-10-07-quad-vehicle-recording-design.md). The render backend keeps a view (its
// targets, passes and histories) for each, in order, for as long as the frames name it.
struct SceneCaptureView
{
    // Its exposure settings are the viewport camera's; the backend adapts its EV on its own.
    Camera camera;
    // View and projections at extent, as the viewport's are made (UpdateViewportMatrices).
    ViewportMatrices matrices;
    RenderExtent extent{};
};
}
