#include "scene_io_service.h"

#include "scene_renderables.h"

#include <engine/editor/renderer_shared_state.h>

#include <engine/asset/asset_registry.h>
#include <engine/core/log/log.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/model_loader.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>

namespace me
{

namespace
{
std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, errorCode);
    return errorCode ? path.lexically_normal() : absolutePath.lexically_normal();
}

bool IsSceneAssetPath(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    if (extension != ".yaml" && extension != ".yml")
    {
        return false;
    }

    const std::string fileName = path.filename().string();
    if (fileName.ends_with(".material.yaml") || fileName.ends_with(".miniengine_asset.yaml"))
    {
        return false;
    }

    return true;
}

// Load side: the uuid recorded in the scene file wins over the stored path,
// so references survive asset renames/moves done since the scene was saved.
void ResolveSerializedReference(std::string& path, std::string& uuid, const char* what)
{
    if (path.empty() && uuid.empty())
    {
        return;
    }

    const ResolvedAssetReference resolved = AssetRegistry::ResolveReference(uuid, path);
    if (resolved.resolved)
    {
        if (resolved.healed)
        {
            LOG_INFO("Healed {} reference via asset registry: '{}' -> '{}'", what, path, resolved.path);
        }
        path = resolved.path;
        uuid = resolved.uuid;
    }
    else if (!path.empty())
    {
        LOG_WARN("Unresolved {} reference: '{}' (uuid '{}')", what, path, uuid);
    }
}

// Save side: a live path is the source of truth, so its uuid is refreshed from
// the registry; a path that went stale while the scene was open (e.g. the
// asset was renamed) is recovered from the uuid recorded at load time.
void EnrichReferenceForSave(std::string& path, std::string& uuid, const char* what)
{
    if (path.empty())
    {
        uuid.clear();
        return;
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec)
    {
        const std::string freshUuid = AssetRegistry::GetOrCreateUuid(path);
        if (!freshUuid.empty())
        {
            uuid = freshUuid;
        }
        return;
    }

    if (!uuid.empty())
    {
        if (const std::optional<std::filesystem::path> healedPath = AssetRegistry::ResolveUuid(uuid))
        {
            LOG_INFO("Healed stale {} path on save: '{}' -> '{}'", what, path, healedPath->string());
            path = healedPath->string();
            return;
        }
    }

    LOG_WARN("Saving scene with missing {} asset: '{}'", what, path);
}

void ResolveSceneAssetReferences(SerializedSceneData& sceneData)
{
    for (SerializedEntityData& entity : sceneData.entities)
    {
        ResolveSerializedReference(entity.modelSourcePath, entity.modelSourceUuid, "model");
        ResolveSerializedReference(
            entity.modelBaseColorTextureOverridePath,
            entity.modelBaseColorTextureOverrideUuid,
            "texture override");
    }
}

bool SceneDataReferencesModel(SerializedSceneData& sceneData, const std::filesystem::path& modelPath)
{
    bool referenced = false;
    for (SerializedEntityData& entity : sceneData.entities)
    {
        if (entity.modelSourcePath.empty())
        {
            continue;
        }

        if (NormalizePath(entity.modelSourcePath) != modelPath)
        {
            continue;
        }

        referenced = true;
        entity.modelDisplayName = modelPath.filename().string();
    }

    return referenced;
}
}

namespace SceneIoService
{
void StartAsyncSceneLoad(RendererSharedState& state, const std::string& path)
{
    if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
    {
        throw std::runtime_error("Another load is currently in progress. Please wait.");
    }

    AsyncSceneLoad& load = state.asyncSceneLoad;
    load.path = path;
    load.progress = std::make_shared<std::atomic<float>>(0.0f);

    // Parse the scene file and pre-warm the model cache for every referenced model
    // in the background; ApplySceneData/RebuildSceneRenderables still run on the main
    // thread once this is ready, but they'll hit the cache instead of parsing on the UI thread.
    load.future = std::async(std::launch::async, [path, progress = load.progress]() -> SerializedSceneData
                             {
                                 constexpr float kSceneFileParsedFraction = 0.1f;

                                 SerializedSceneData sceneData = LoadEditorSceneDataFromFile(path);
                                 ResolveSceneAssetReferences(sceneData);
                                 progress->store(kSceneFileParsedFraction);

                                 std::vector<std::string> modelPathsToLoad;
                                 for (const SerializedEntityData& entity : sceneData.entities)
                                 {
                                     const std::string& modelPath = entity.modelSourcePath;
                                     const bool alreadyQueued =
                                         std::find(modelPathsToLoad.begin(), modelPathsToLoad.end(), modelPath) != modelPathsToLoad.end();
                                     if (!modelPath.empty() && !alreadyQueued && !ModelCache::IsCached(modelPath))
                                     {
                                         modelPathsToLoad.push_back(modelPath);
                                     }
                                 }

                                 // Each pending model gets an equal slice of the remaining progress range.
                                 const float modelSlice = modelPathsToLoad.empty()
                                                              ? 0.0f
                                                              : (1.0f - kSceneFileParsedFraction) / static_cast<float>(modelPathsToLoad.size());
                                 for (size_t modelIndex = 0; modelIndex < modelPathsToLoad.size(); ++modelIndex)
                                 {
                                     const std::string& modelPath = modelPathsToLoad[modelIndex];
                                     const float sliceStart = kSceneFileParsedFraction + modelSlice * static_cast<float>(modelIndex);
                                     auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(
                                         modelPath,
                                         [&progress, sliceStart, modelSlice](float fraction)
                                         {
                                             progress->store(sliceStart + modelSlice * fraction);
                                         }));
                                     ModelCache::Store(modelPath, std::move(data));
                                 }

                                 progress->store(1.0f);
                                 return sceneData;
                             });

    LOG_INFO("Started async load for scene: {}", path);
}

bool PumpAsyncSceneLoad(RendererSharedState& state)
{
    AsyncSceneLoad& sceneLoad = state.asyncSceneLoad;
    if (!sceneLoad.future.valid() ||
        sceneLoad.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }

    bool renderablesDirty = false;
    try
    {
        const SerializedSceneData sceneData = sceneLoad.future.get(); // re-throws on background failure
        state.GetEditorWorld().ApplySceneData(sceneData);
        RebuildSceneRenderables(state);
        state.GetEditorWorld().SetSceneFilePath(sceneLoad.path);
        state.lastSceneIoError.clear();
        renderablesDirty = true;
        LOG_INFO("Loaded scene successfully: {}", sceneLoad.path);
    }
    catch (const std::exception& error)
    {
        state.lastSceneIoError = error.what();
        LOG_ERROR("Failed to load scene '{}': {}", sceneLoad.path, error.what());
    }

    sceneLoad.future = std::future<SerializedSceneData>{}; // consume / reset
    state.lastFrameTime = std::chrono::steady_clock::now();
    return renderablesDirty;
}

void SaveScene(RendererSharedState& state, const std::string& path)
{
    SerializedSceneData sceneData = state.GetEditorWorld().CaptureSceneData();
    for (SerializedEntityData& entity : sceneData.entities)
    {
        EnrichReferenceForSave(entity.modelSourcePath, entity.modelSourceUuid, "model");
        EnrichReferenceForSave(
            entity.modelBaseColorTextureOverridePath,
            entity.modelBaseColorTextureOverrideUuid,
            "texture override");
    }

    SaveEditorSceneDataToFile(sceneData, path);
    state.GetEditorWorld().SetSceneFilePath(path);
    state.lastSceneIoError.clear();
    LOG_INFO("Saved scene successfully: {}", path);
}

size_t RefreshReferencedSceneFiles(const std::filesystem::path& modelPath)
{
    size_t refreshedSceneCount = 0;
    std::error_code iteratorError;
    const std::filesystem::path workspaceRoot = NormalizePath(std::filesystem::current_path());

    for (std::filesystem::recursive_directory_iterator iterator(workspaceRoot, iteratorError), end;
         !iteratorError && iterator != end;
         iterator.increment(iteratorError))
    {
        if (!iterator->is_regular_file(iteratorError) || iteratorError)
        {
            continue;
        }

        const std::filesystem::path candidatePath = NormalizePath(iterator->path());
        if (!IsSceneAssetPath(candidatePath))
        {
            continue;
        }

        try
        {
            SerializedSceneData sceneData = LoadEditorSceneDataFromFile(candidatePath.string());
            if (!SceneDataReferencesModel(sceneData, modelPath))
            {
                continue;
            }

            SaveEditorSceneDataToFile(sceneData, candidatePath.string());
            ++refreshedSceneCount;
        }
        catch (...)
        {
            // Ignore non-scene YAML files and malformed sidecar data while scanning the workspace.
        }
    }

    return refreshedSceneCount;
}

}
}
