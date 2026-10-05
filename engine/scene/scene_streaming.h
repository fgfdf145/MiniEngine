#pragma once

#include <string>

namespace me
{

// A streamed world a scene draws from (docs/design/2026-10-05-world-streaming-design.md): a manifest of
// cells, each with a high-detail and a LOD model, of which WorldStreamingService loads the high-detail
// ones near the focus and the LOD ones elsewhere.
struct SceneStreamingWorld
{
    // The manifest's path, as model paths are written (relative to the project root).
    std::string manifest;
    // A cell's high detail loads once the focus is this close to its bounds (metres, across the ground),
    // and goes back to its LOD once it is farther than unloadRadius.
    float loadRadius = 700.0f;
    float unloadRadius = 900.0f;

    bool operator==(const SceneStreamingWorld&) const = default;
};
}
