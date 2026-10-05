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

glm::vec3 ReadVec3(const YAML::Node& node)
{
    if (!node || !node.IsSequence() || node.size() != 3)
    {
        throw std::runtime_error("a bounds entry is not three numbers");
    }
    return glm::vec3(node[0].as<float>(), node[1].as<float>(), node[2].as<float>());
}

std::vector<StreamedCell> ReadCells(const SceneStreamingWorld& world)
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
        cells.push_back(std::move(cell));
    }
    return cells;
}

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
}

float WorldStreamingService::HorizontalDistance(const glm::vec3& point, const glm::vec3& boundsMin, const glm::vec3& boundsMax)
{
    const float dx = std::max({boundsMin.x - point.x, 0.0f, point.x - boundsMax.x});
    const float dz = std::max({boundsMin.z - point.z, 0.0f, point.z - boundsMax.z});
    return std::sqrt(dx * dx + dz * dz);
}

StreamedCell::Shown WorldStreamingService::TargetOf(const StreamedCell& cell)
{
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
                std::vector<StreamedCell> cells = ReadCells(streamed);
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
        streaming.settled = true;
        if (changed)
        {
            RefreshDirtySceneRenderables(state);
        }
        return changed;
    }

    // What each cell wants, nearest first.
    const glm::vec3 focus = FocusOf(state);
    std::vector<std::pair<float, size_t>> pending;
    for (size_t index = 0; index < streaming.cells.size(); ++index)
    {
        StreamedCell& cell = streaming.cells[index];
        const float distance = HorizontalDistance(focus, cell.boundsMin, cell.boundsMax);
        cell.wantHighDetail = cell.wantHighDetail ? distance <= cell.unloadRadius : distance <= cell.loadRadius;
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
    const auto now = std::chrono::steady_clock::now();
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
                streaming.load = std::async(std::launch::async, [path]()
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
