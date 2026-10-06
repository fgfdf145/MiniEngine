#pragma once

#include <engine/scene/scene_streaming.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace me
{

struct RendererSharedState;

// One cell of a streamed world's manifest, and what of it is in the scene now.
struct StreamedCell
{
    enum class Shown
    {
        None,
        Lod,
        HighDetail,
    };

    std::string name;
    // Model paths as entities write them; either may be empty.
    std::string highDetailPath;
    std::string lodPath;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    float loadRadius = 700.0f;
    float unloadRadius = 900.0f;
    // The manifest's far_only: the cell shows its LOD only from beyond its radius and nothing within it.
    // For a LOD that other cells replace up close, such as GTA III's whole-island models, whose bounds
    // are the island's: whenever any cell of the island is in high detail, the island's LOD is gone.
    bool farOnly = false;

    bool wantHighDetail = false;
    Shown shown = Shown::None;
    entt::entity entity = entt::null;
};

// The scene's streamed worlds (SceneStreamingWorld) as WorldStreamingService runs them.
struct WorldStreamingState
{
    // The worlds the cells were read for: a scene that names others reads them again.
    std::vector<SceneStreamingWorld> worlds;
    std::vector<StreamedCell> cells;
    // The model being parsed in the background, one at a time (the model loader does not take two
    // reads of one file at once, and one keeps the disk to the nearest cell first).
    std::future<void> load;
    std::string loadingPath;
    std::unordered_set<std::string> failedPaths;
    // Every cell shows what the focus wants of it, and nothing is loading.
    bool settled = true;
    std::string lastError;
    // When cells last swapped between high detail and LOD (see kSwapBatchSeconds).
    std::chrono::steady_clock::time_point lastSwap{};

    // The cells showing their high detail, and those showing their LOD.
    size_t HighDetailCount() const;
    size_t LodCount() const;
};

// docs/design/2026-10-05-world-streaming-design.md: each frame, a cell within its load radius of the
// focus (the driven car, else the camera) shows its high-detail model, one beyond its unload radius its
// LOD model. A model is parsed in the background first; once it is in the model cache the cell's entity
// is swapped (the new one made, the old one destroyed, in the same frame), so the render backend commits
// both together once the new textures are ready.
namespace WorldStreamingService
{
// True when it changed the scene's renderables.
bool Tick(RendererSharedState& state);

// The cells of a streamed world's manifest (docs/design/2026-10-05-world-streaming-design.md), their
// model paths resolved against the manifest's folder. Throws when the manifest cannot be read.
std::vector<StreamedCell> ReadCells(const SceneStreamingWorld& world);

// Horizontal distance from a point to a box: what the radii are measured by.
float HorizontalDistance(const glm::vec3& point, const glm::vec3& boundsMin, const glm::vec3& boundsMax);

// The shown state a cell should move to, given whether it wants its high detail.
StreamedCell::Shown TargetOf(const StreamedCell& cell);
}
}
