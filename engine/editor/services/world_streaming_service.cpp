#include "world_streaming_service.h"

#include <engine/asset/model_cache.h>
#include <engine/asset/model_loader.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/scene_renderables.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

namespace me
{

size_t WorldStreamingState::HighDetailCount() const
{
    return static_cast<size_t>(std::count_if(cells.begin(), cells.end(), [](const StreamedCell& cell)
                                             {
                                                 return cell.shown == StreamedCell::Shown::HighDetail;
                                             }));
}

size_t WorldStreamingState::LodCount() const
{
    return static_cast<size_t>(std::count_if(cells.begin(), cells.end(), [](const StreamedCell& cell)
                                             {
                                                 return cell.shown == StreamedCell::Shown::Lod;
                                             }));
}

namespace
{
// Entity swaps a frame: each costs a renderable rebuild of its cell, so the first fill of a few hundred
// LOD cells is spread over frames instead of stalling one.
constexpr int kMaxSwapsPerFrame = 16;
// Every change of what is drawn costs the render backend a commit of tens of milliseconds whatever its
// size, so swaps between high detail and LOD wait until this long after the last and then go together.
// A cell that shows nothing yet (the first fill) never waits.
constexpr float kSwapBatchSeconds = 0.5f;

// The GPU memory budget (docs/design/2026-10-07-vram-budget-design.md). The radius grows when more than
// kGrowHeadroom is free and shrinks when nothing is, each time to leave about kTargetHeadroom: the band
// between them absorbs a wrong guess of the cost of a cell.
constexpr int64_t kMiB = int64_t{1} << 20;
constexpr int64_t kGrowHeadroom = 384 * kMiB;
constexpr int64_t kTargetHeadroom = 192 * kMiB;
// A cell's cost before any is loaded to measure it by (a GTA SA cell is 10-30 MB).
constexpr int64_t kDefaultCellBytes = 48 * kMiB;
// Steps come at most this often, which gives the measurement time to follow.
constexpr float kBudgetStepSeconds = 1.0f;
// How long a dip of the driver's budget is remembered. On Windows the budget rises when other
// processes' memory could be paged out for ours and falls again once ours grows (seen 7.2 -> 5.3 GB
// within seconds, a cycle of 20-30 s), so a dip is the truth for a while.
constexpr std::chrono::seconds kBudgetWindow{60};
// After a shrink the radius does not grow for this long, which damps the cycle above.
constexpr std::chrono::seconds kGrowAfterShrink{20};

glm::vec3 ReadVec3(const YAML::Node& node)
{
    if (!node || !node.IsSequence() || node.size() != 3)
    {
        throw std::runtime_error("a bounds entry is not three numbers");
    }
    return glm::vec3(node[0].as<float>(), node[1].as<float>(), node[2].as<float>());
}
}

std::vector<StreamedCell> WorldStreamingService::ReadCells(const SceneStreamingWorld& world)
{
    const std::filesystem::path manifestPath = EnginePaths::ResolveProjectPath(world.manifest);
    const YAML::Node root = YAML::LoadFile(manifestPath.string());
    // Cell models are written relative to the manifest. They are loaded from there, wherever the
    // editor was started from, so they are kept resolved, not in the manifest's project-relative form.
    const std::filesystem::path base = manifestPath.parent_path();
    const auto modelPath = [&base](const YAML::Node& node) -> std::string
    {
        return node ? (base / node.as<std::string>()).lexically_normal().generic_string() : std::string{};
    };
    std::vector<StreamedCell> cells;
    for (const YAML::Node& node : root["cells"])
    {
        StreamedCell cell;
        cell.name = node["name"].as<std::string>("");
        cell.highDetailPath = modelPath(node["hd"]);
        cell.lodPath = modelPath(node["lod"]);
        cell.boundsMin = ReadVec3(node["bounds_min"]);
        cell.boundsMax = ReadVec3(node["bounds_max"]);
        cell.loadRadius = world.loadRadius;
        cell.unloadRadius = world.unloadRadius;
        cell.farOnly = node["far_only"].as<bool>(false);
        if (world.highDetailOnly)
        {
            // Within any distance: the high detail always, the LOD never (a far-only one is gone too).
            cell.lodPath.clear();
            cell.loadRadius = std::numeric_limits<float>::infinity();
            cell.unloadRadius = std::numeric_limits<float>::infinity();
        }
        cells.push_back(std::move(cell));
    }
    return cells;
}

namespace
{
void DestroyCellEntity(IEditorWorld& world, StreamedCell& cell)
{
    if (cell.entity != entt::null && world.IsValidEntity(cell.entity))
    {
        world.DestroyEntity(cell.entity);
    }
    cell.entity = entt::null;
    cell.shown = StreamedCell::Shown::None;
}

glm::vec3 FocusOf(RendererSharedState& state)
{
    IEditorWorld& world = state.GetEditorWorld();
    if (const VehicleDriveSession* session = state.vehicleDrive.session.get(); session != nullptr && world.IsValidEntity(session->entity))
    {
        return glm::vec3(world.GetModelMatrix(session->entity)[3]);
    }
    return state.camera.position;
}

// One step of the budget radius (NextBudgetRadius), at most every kBudgetStepSeconds.
void UpdateBudgetRadius(RendererSharedState& state, const std::vector<float>& distances, std::chrono::steady_clock::time_point now)
{
    WorldStreamingState& streaming = state.worldStreaming;
    if (streaming.budgetRadius <= 0.0f)
    {
        streaming.budgetRadius = WorldStreamingService::kMinBudgetRadius;
    }
    // The driver's budget moves with what other processes take: the lowest of the last seconds holds.
    if (state.gpuMemory.serial != 0 &&
        (streaming.budgetSamples.empty() || now - streaming.budgetSamples.back().first >= std::chrono::milliseconds(250)))
    {
        streaming.budgetSamples.emplace_back(now, state.gpuMemory.budget);
        while (now - streaming.budgetSamples.front().first > kBudgetWindow)
        {
            streaming.budgetSamples.pop_front();
        }
    }
    if (std::chrono::duration<float>(now - streaming.lastBudgetChange).count() < kBudgetStepSeconds)
    {
        return;
    }
    std::vector<float> highDetail;
    std::vector<float> candidates;
    for (size_t index = 0; index < streaming.cells.size(); ++index)
    {
        const StreamedCell& cell = streaming.cells[index];
        if (cell.shown == StreamedCell::Shown::HighDetail)
        {
            highDetail.push_back(distances[index]);
        }
        else if (!cell.highDetailPath.empty() && !cell.farOnly && distances[index] <= cell.loadRadius &&
                 streaming.failedPaths.count(cell.highDetailPath) == 0)
        {
            candidates.push_back(distances[index]);
        }
    }
    GpuMemoryReport report = state.gpuMemory;
    for (const auto& [time, budget] : streaming.budgetSamples)
    {
        report.budget = std::min(report.budget, budget);
    }
    const bool outOfMemory = std::exchange(streaming.uploadOutOfMemory, false);
    // Growing waits for the last change to be drawn, so that the cost of a cell is measured on cells
    // that are all on the GPU. Shrinking never waits: the cells on their way are counted.
    const bool mayGrow = streaming.settled && !state.rayScenePending && now - streaming.lastBudgetShrink >= kGrowAfterShrink;
    const float radius = WorldStreamingService::NextBudgetRadius(
        streaming.budgetRadius, report, outOfMemory, mayGrow, std::move(highDetail), std::move(candidates));
    if (radius == streaming.budgetRadius)
    {
        return;
    }
    LOG_INFO(
        "GPU memory: {} of {} MB used (world {} MB, fullscreen reserve {} MB){}: streaming radius {:.0f} -> {:.0f} m",
        report.usage >> 20,
        report.budget >> 20,
        report.worldBytes >> 20,
        report.reserve >> 20,
        outOfMemory ? ", an upload ran out of memory" : "",
        streaming.budgetRadius,
        radius);
    if (radius < streaming.budgetRadius)
    {
        streaming.lastBudgetShrink = now;
    }
    streaming.budgetRadius = radius;
    streaming.lastBudgetChange = now;
}
}

float WorldStreamingService::HorizontalDistance(const glm::vec3& point, const glm::vec3& boundsMin, const glm::vec3& boundsMax)
{
    const float dx = std::max({boundsMin.x - point.x, 0.0f, point.x - boundsMax.x});
    const float dz = std::max({boundsMin.z - point.z, 0.0f, point.z - boundsMax.z});
    return std::sqrt(dx * dx + dz * dz);
}

float WorldStreamingService::NextBudgetRadius(
    float radius,
    const GpuMemoryReport& report,
    bool outOfMemory,
    bool mayGrow,
    std::vector<float> highDetailDistances,
    std::vector<float> candidateDistances)
{
    if (report.serial == 0 && !outOfMemory)
    {
        return radius;
    }
    const int64_t cellBytes = highDetailDistances.empty()
                                  ? kDefaultCellBytes
                                  : std::max(kMiB, static_cast<int64_t>(report.worldBytes / highDetailDistances.size()));

    // Where the radius already leads: cells beyond it are on their way out, cells within it that are
    // not shown yet on their way in. Neither shows in the measurement yet, so they are counted here;
    // that lets a step follow the last without waiting for its uploads and releases.
    std::vector<float> future;
    std::vector<float> beyond;
    int64_t leaving = 0;
    int64_t arriving = 0;
    for (const float distance : highDetailDistances)
    {
        if (distance <= radius + kBudgetRadiusHysteresis)
        {
            future.push_back(distance);
        }
        else
        {
            ++leaving;
        }
    }
    for (const float distance : candidateDistances)
    {
        if (distance <= radius)
        {
            future.push_back(distance);
            ++arriving;
        }
        else
        {
            beyond.push_back(distance);
        }
    }
    std::sort(future.begin(), future.end());
    const int64_t headroom = report.Headroom() + (leaving - arriving) * cellBytes;

    if (outOfMemory || headroom < 0)
    {
        // The farthest cells go, as many as the overshoot is worth; after a failed upload at least an
        // eighth of them, as the failed upload's own size is unknown.
        const size_t count = future.size();
        size_t drop = headroom < 0 ? static_cast<size_t>((-headroom + kTargetHeadroom + cellBytes - 1) / cellBytes) : 0;
        if (outOfMemory)
        {
            drop = std::max(drop, std::max<size_t>(1, count / 8));
        }
        drop = std::min(drop, count);
        if (drop == 0)
        {
            return radius;
        }
        // Below the first cell to go by more than the hysteresis, which would otherwise keep it.
        const float firstDropped = future[count - drop];
        return std::max(kMinBudgetRadius, std::min(radius, firstDropped - kBudgetRadiusHysteresis - 1.0f));
    }

    if (mayGrow && headroom > kGrowHeadroom && !beyond.empty())
    {
        // Half the room at a time: the nearest cells measured the cost, and the next ones may cost more.
        std::sort(beyond.begin(), beyond.end());
        const size_t add = std::clamp<size_t>(static_cast<size_t>((headroom - kTargetHeadroom) / cellBytes / 2), 1, beyond.size());
        return std::max(radius, beyond[add - 1]);
    }
    return radius;
}

StreamedCell::Shown WorldStreamingService::TargetOf(const StreamedCell& cell)
{
    if (cell.wantHighDetail && cell.farOnly)
    {
        return StreamedCell::Shown::None;
    }
    if (cell.wantHighDetail && !cell.highDetailPath.empty())
    {
        return StreamedCell::Shown::HighDetail;
    }
    return cell.lodPath.empty() ? StreamedCell::Shown::None : StreamedCell::Shown::Lod;
}

bool WorldStreamingService::Tick(RendererSharedState& state)
{
    if (!state.editorWorld)
    {
        return false;
    }
    IEditorWorld& world = state.GetEditorWorld();
    WorldStreamingState& streaming = state.worldStreaming;

    // A finished background parse: the model is in the cache now, or it failed and is not asked again.
    if (streaming.load.valid() && streaming.load.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            streaming.load.get();
        }
        catch (const std::exception& error)
        {
            streaming.failedPaths.insert(streaming.loadingPath);
            streaming.lastError = error.what();
            LOG_ERROR("Streaming could not load '{}': {}", streaming.loadingPath, error.what());
        }
        streaming.load = {};
        streaming.loadingPath.clear();
    }

    // A loaded scene replaced every entity, ours with them; a scene naming other worlds starts over.
    const std::vector<SceneStreamingWorld>& worlds = world.GetStreamingWorlds();
    for (StreamedCell& cell : streaming.cells)
    {
        if (cell.entity != entt::null && !world.IsValidEntity(cell.entity))
        {
            cell.entity = entt::null;
            cell.shown = StreamedCell::Shown::None;
        }
    }
    bool changed = false;
    if (worlds != streaming.worlds)
    {
        for (StreamedCell& cell : streaming.cells)
        {
            changed |= cell.entity != entt::null;
            DestroyCellEntity(world, cell);
        }
        streaming.cells.clear();
        streaming.failedPaths.clear();
        streaming.lastError.clear();
        streaming.worlds = worlds;
        for (const SceneStreamingWorld& streamed : worlds)
        {
            try
            {
                std::vector<StreamedCell> cells = WorldStreamingService::ReadCells(streamed);
                LOG_INFO("Streaming {} cells from '{}'", cells.size(), streamed.manifest);
                streaming.cells.insert(streaming.cells.end(), std::make_move_iterator(cells.begin()), std::make_move_iterator(cells.end()));
            }
            catch (const std::exception& error)
            {
                streaming.lastError = "Cannot read the streaming manifest '" + streamed.manifest + "': " + error.what();
                LOG_ERROR("{}", streaming.lastError);
            }
        }
    }
    if (streaming.cells.empty())
    {
        // Nothing to give back: a failed upload is the scene's own, not a world's to shed cells for.
        streaming.uploadOutOfMemory = false;
        streaming.settled = true;
        if (changed)
        {
            RefreshDirtySceneRenderables(state);
        }
        return changed;
    }

    const glm::vec3 focus = FocusOf(state);
    std::vector<float> distances(streaming.cells.size());
    for (size_t index = 0; index < streaming.cells.size(); ++index)
    {
        const StreamedCell& cell = streaming.cells[index];
        distances[index] = HorizontalDistance(focus, cell.boundsMin, cell.boundsMax);
    }
    const auto now = std::chrono::steady_clock::now();
    UpdateBudgetRadius(state, distances, now);

    // What each cell wants, nearest first: its high detail within its own radius and the budget's.
    std::vector<std::pair<float, size_t>> pending;
    for (size_t index = 0; index < streaming.cells.size(); ++index)
    {
        StreamedCell& cell = streaming.cells[index];
        const float distance = distances[index];
        cell.wantHighDetail = cell.wantHighDetail
                                  ? distance <= std::min(cell.unloadRadius, streaming.budgetRadius + kBudgetRadiusHysteresis)
                                  : distance <= std::min(cell.loadRadius, streaming.budgetRadius);
        if (TargetOf(cell) != cell.shown)
        {
            pending.emplace_back(distance, index);
        }
    }
    std::sort(pending.begin(), pending.end());

    // The scene loader and the single-model loader parse on their own threads; the cache warms one
    // model at a time, so streaming waits for them rather than read alongside.
    const bool otherLoaderBusy = state.asyncSceneLoad.IsLoading() || state.asyncLoad.IsLoading();
    int swaps = 0;
    const bool batchDue = std::chrono::duration<float>(now - streaming.lastSwap).count() >= kSwapBatchSeconds;
    for (const auto& [distance, index] : pending)
    {
        StreamedCell& cell = streaming.cells[index];
        StreamedCell::Shown target = TargetOf(cell);
        if (target == StreamedCell::Shown::HighDetail && streaming.failedPaths.count(cell.highDetailPath) != 0)
        {
            target = cell.lodPath.empty() ? StreamedCell::Shown::None : StreamedCell::Shown::Lod;
        }
        if (target == cell.shown)
        {
            continue;
        }
        if (target == StreamedCell::Shown::None)
        {
            DestroyCellEntity(world, cell);
            changed = true;
            continue;
        }
        const std::string& path = target == StreamedCell::Shown::HighDetail ? cell.highDetailPath : cell.lodPath;
        if (streaming.failedPaths.count(path) != 0)
        {
            continue;
        }
        if (!ModelCache::IsCached(path))
        {
            if (!streaming.load.valid() && !otherLoaderBusy)
            {
                streaming.loadingPath = path;
                streaming.load = RunAsync(TaskPriority::Low, [path]()
                                          {
                                              auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(path));
                                              ModelCache::Store(path, std::move(data));
                                          });
            }
            continue;
        }
        if (swaps >= kMaxSwapsPerFrame || (cell.shown != StreamedCell::Shown::None && !batchDue))
        {
            continue;
        }

        SerializedEntityData entityData;
        entityData.tagName = target == StreamedCell::Shown::HighDetail ? cell.name : cell.name + " (LOD)";
        entityData.modelDisplayName = entityData.tagName;
        entityData.modelSourcePath = path;
        const entt::entity created = world.CreateStreamedEntity(entityData, StreamedComponent{cell.name, target == StreamedCell::Shown::Lod});
        DestroyCellEntity(world, cell);
        cell.entity = created;
        cell.shown = target;
        ++swaps;
        changed = true;
        streaming.lastSwap = now;
    }

    streaming.settled = streaming.cells.end() == std::find_if(streaming.cells.begin(), streaming.cells.end(), [&](const StreamedCell& cell)
                                                              {
                                                                  StreamedCell::Shown target = TargetOf(cell);
                                                                  if (target == StreamedCell::Shown::HighDetail &&
                                                                      streaming.failedPaths.count(cell.highDetailPath) != 0)
                                                                  {
                                                                      target = cell.lodPath.empty() ? StreamedCell::Shown::None : StreamedCell::Shown::Lod;
                                                                  }
                                                                  return target != cell.shown &&
                                                                         streaming.failedPaths.count(target == StreamedCell::Shown::HighDetail ? cell.highDetailPath : cell.lodPath) == 0;
                                                              }) &&
                        !streaming.load.valid();

    if (changed)
    {
        RefreshDirtySceneRenderables(state);
        LOG_INFO(
            "Streaming: {} cells in high detail, {} in LOD{}",
            streaming.HighDetailCount(),
            streaming.LodCount(),
            streaming.settled ? "" : " (changing)");
    }
    return changed;
}
}
