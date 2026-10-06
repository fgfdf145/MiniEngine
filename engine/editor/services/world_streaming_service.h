#pragma once

#include <engine/core/threading/task_future.h>
#include <engine/renderer/render_types.h>
#include <engine/scene/scene_streaming.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
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
    TaskFuture<void> load;
    std::string loadingPath;
    std::unordered_set<std::string> failedPaths;
    // Every cell shows what the focus wants of it, and nothing is loading.
    bool settled = true;
    std::string lastError;
    // When cells last swapped between high detail and LOD (see kSwapBatchSeconds).
    std::chrono::steady_clock::time_point lastSwap{};
    // docs/design/2026-10-07-vram-budget-design.md: no cell beyond this distance from the focus is in
    // high detail, whatever its own radius, so that the world fits in the GPU's memory budget.
    float budgetRadius = 0.0f;
    std::chrono::steady_clock::time_point lastBudgetChange{};
    std::chrono::steady_clock::time_point lastBudgetShrink{};
    // The driver's budget over the last seconds, by when it was read.
    std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> budgetSamples;
    // Set by the render backend when an upload ran out of GPU memory: the radius shrinks next.
    bool uploadOutOfMemory = false;

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

// The budget radius never goes below this: the cells around the focus always load, over budget or not.
inline constexpr float kMinBudgetRadius = 150.0f;
// A cell in high detail stays until it is this far beyond the budget radius (no flicker at its edge).
inline constexpr float kBudgetRadiusHysteresis = 25.0f;

// One step of the budget radius (docs/design/2026-10-07-vram-budget-design.md). highDetailDistances: the
// distances of the cells in high detail now; candidateDistances: those of the cells that would load
// their high detail within their own radius but do not show it. The report's headroom is corrected for
// the cells the radius already sends away or brings in, at the world's bytes per high-detail cell.
// Shrinks when that is negative or an upload ran out of memory, grows by half the room when there is
// room for more cells (only with mayGrow), else returns radius unchanged.
float NextBudgetRadius(
    float radius,
    const GpuMemoryReport& report,
    bool outOfMemory,
    bool mayGrow,
    std::vector<float> highDetailDistances,
    std::vector<float> candidateDistances);
}
}
