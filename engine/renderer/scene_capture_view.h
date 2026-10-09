#pragma once

#include "camera.h"
#include "path_tracing.h"
#include "render_types.h"

#include <optional>

namespace me
{

// A camera the scene is drawn from in a frame besides the viewport's: one of a quad recording's
// (docs/design/2026-10-07-quad-vehicle-recording-design.md), or Photo Mode's
// (docs/design/2026-10-09-photo-mode-design.md), which comes after them. The render backend keeps a
// view (its targets, passes and histories) for each, in order, for as long as the frames name it.
struct SceneCaptureView
{
    // Its exposure settings are the viewport camera's; the backend adapts its EV on its own.
    Camera camera;
    // View and projections at extent, as the viewport's are made (UpdateViewportMatrices).
    ViewportMatrices matrices;
    RenderExtent extent{};
    // Photo Mode's view: not a quad camera, and the one a photo is read back from.
    bool photo = false;
    // The whole image when this view renders one tile of it (a Photo Mode tile, its matrices an
    // off-centre part of the whole's frustum): its shadow cascades are fitted to the whole's frustum
    // and its glare spread at the whole's pixel pitch, so the tiles agree. Empty for a view that is
    // its own image.
    RenderExtent wholeExtent{};
    // The view starts over this frame (a new tile): its temporal histories are dropped.
    bool resetHistory = false;

    // Photo Mode's renderer (docs/design/2026-10-09-photo-offline-path-tracing-design.md): the path
    // tracer's offline mode (at the viewport's path_tracing_offline settings) rather than the
    // rasterised pipeline with the traced effects, and DLSS of its own, at this mode and model, with
    // ray reconstruction where it runs (Off: the engine's TAA). Quad views keep their defaults.
    bool offlinePathTracing = false;
    // The offline mode's samples a frame and in all, a pixel (path_tracing_offline's own are the
    // viewport's).
    uint32_t samplesPerPixel = 2;
    uint32_t targetSamples = 1024;
    DlssMode dlssMode = DlssMode::Off;
    DlssPreset dlssPreset = DlssPreset::Default;
    bool dlssRayReconstruction = false;
    // Which of the photo's tiles this is, echoed back with the path tracer's progress, so the photo
    // never takes a tile's picture on the last tile's progress.
    uint32_t photoTile = 0;
};

// What Photo Mode's view did in the frame the backend drew last: the tile it rendered, how far its
// offline path tracing is (unset where it did not path trace), and how it resolved. A photo takes a
// path traced tile once this says the tile's image has all its samples.
struct PhotoViewReport
{
    uint32_t tile = 0;
    std::optional<OfflineProgress> offline;
    DlssMode dlss = DlssMode::Off;
    bool rayReconstruction = false;
};
}
