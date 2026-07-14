#include "scene_io_service.h"

#include "scene_renderables.h"

#include <renderer_shared_state.h>

#include <log/log.h>
#include <model_cache.h>
#include <model_loader.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>

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

    // Parse the scene file and pre-warm the model cache for every referenced model
    // in the background; ApplySceneData/RebuildSceneRenderables still run on the main
    // thread once this is ready, but they'll hit the cache instead of parsing on the UI thread.
    load.future = std::async(std::launch::async, [path]() -> SerializedSceneData
    {
        SerializedSceneData sceneData = LoadEditorSceneDataFromFile(path);

        for (const SerializedEntityData& entity : sceneData.entities)
        {
            if (entity.modelSourcePath.empty() || ModelCache::IsCached(entity.modelSourcePath))
            {
                continue;
            }
            auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(entity.modelSourcePath));
            ModelCache::Store(entity.modelSourcePath, std::move(data));
        }

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
    state.GetEditorWorld().SaveSceneToFile(path);
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
