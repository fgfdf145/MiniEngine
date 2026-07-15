#include "entity_edit_service.h"

#include "scene_renderables.h"

#include <renderer_shared_state.h>

#include <asset_registry.h>
#include <log/log.h>
#include <model_cache.h>
#include <model_loader.h>

#include <algorithm>
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
}

namespace EntityEditService
{
void LoadSelectedModel(RendererSharedState& state, const std::string& path, bool resetTransform)
{
    if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
    {
        throw std::runtime_error("Another load is currently in progress. Please wait.");
    }

    if (!state.GetEditorWorld().HasSelection())
    {
        throw std::runtime_error("No selected entity available to receive the model");
    }

    const entt::entity selectedEntity = state.GetEditorWorld().GetSelectedEntity();
    const ModelComponent previousModel = state.GetEditorWorld().GetModel(selectedEntity);
    state.GetEditorWorld().GetModel(selectedEntity).sourcePath = path;
    state.GetEditorWorld().GetModel(selectedEntity).sourceUuid = AssetRegistry::GetOrCreateUuid(path);
    state.GetEditorWorld().GetModel(selectedEntity).displayName = std::filesystem::path(path).filename().string();

    // Fast path: already cached.
    if (ModelCache::IsCached(path))
    {
        try
        {
            RebuildSceneRenderables(state);
            const ModelComponent& model = state.GetEditorWorld().GetModel(selectedEntity);
            if (model.hasBounds)
            {
                state.camera.FrameBounds(model.minBounds, model.maxBounds);
            }
        }
        catch (...)
        {
            state.GetEditorWorld().GetModel(selectedEntity) = previousModel;
            throw;
        }
        state.lastModelLoadError.clear();
        if (resetTransform)
        {
            state.GetEditorWorld().ResetSelectedTransform();
        }
        LOG_INFO("Loaded model (cached) into '{}': {}", state.GetEditorWorld().GetTag(selectedEntity).name, path);
        return;
    }

    // Async path: parse in background, rebuild when done.
    AsyncModelLoad& load = state.asyncLoad;
    load.path = path;
    load.isReplacement = true;
    load.worldPosition = glm::vec3(0.0f);
    load.trackedEntity = selectedEntity;
    load.previousSelection = entt::null;
    load.resetTransformOnComplete = resetTransform;
    load.previousSourcePath = previousModel.sourcePath;
    load.previousSourceUuid = previousModel.sourceUuid;
    load.previousDisplayName = previousModel.displayName;
    load.progress = std::make_shared<std::atomic<float>>(0.0f);

    load.future = std::async(std::launch::async, [p = path, progress = load.progress]()
    {
        auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(p, [&progress](float fraction)
        {
            progress->store(fraction);
        }));
        ModelCache::Store(p, std::move(data));
        progress->store(1.0f);
    });

    LOG_INFO("Started async load for: {}", path);
}

void PlaceModelIntoScene(RendererSharedState& state, const std::string& path, const glm::vec3& worldPosition)
{
    if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
    {
        throw std::runtime_error("Another load is currently in progress. Please wait.");
    }

    const std::filesystem::path modelPath = NormalizePath(path);
    if (!std::filesystem::exists(modelPath))
    {
        throw std::runtime_error("Dropped model asset does not exist: " + modelPath.string());
    }
    if (!ModelLoader::IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error("Dropped asset is not a supported glTF model: " + modelPath.string());
    }

    SerializedEntityData entityData{};
    entityData.tagName = modelPath.stem().string().empty() ? "Model" : modelPath.stem().string();
    entityData.modelDisplayName = modelPath.filename().string();
    entityData.modelSourcePath = modelPath.string();
    entityData.modelSourceUuid = AssetRegistry::GetOrCreateUuid(modelPath);
    entityData.transform.translation = worldPosition;

    const entt::entity previousSelection =
        state.GetEditorWorld().HasSelection() ? state.GetEditorWorld().GetSelectedEntity() : entt::null;
    const entt::entity placedEntity = state.GetEditorWorld().CreateEntity(entityData);
    state.GetEditorWorld().SetSelectedEntity(placedEntity);

    // If already cached, rebuild synchronously (fast path).
    if (ModelCache::IsCached(modelPath.string()))
    {
        try
        {
            RebuildSceneRenderables(state);
            const ModelComponent& model = state.GetEditorWorld().GetModel(placedEntity);
            if (model.hasBounds)
            {
                state.camera.FrameBounds(model.minBounds, model.maxBounds);
            }
        }
        catch (...)
        {
            state.GetEditorWorld().DestroyEntity(placedEntity);
            state.GetEditorWorld().SetSelectedEntity(previousSelection);
            throw;
        }
        state.lastModelLoadError.clear();
        LOG_INFO("Placed model (cached) at ({:.3f}, {:.3f}, {:.3f}): {}",
            worldPosition.x, worldPosition.y, worldPosition.z, modelPath.string());
        return;
    }

    // Not cached: parse in background, rebuild when done.
    AsyncModelLoad& load = state.asyncLoad;
    load.path = modelPath.string();
    load.isReplacement = false;
    load.worldPosition = worldPosition;
    load.trackedEntity = placedEntity;
    load.previousSelection = previousSelection;
    load.resetTransformOnComplete = false;
    load.previousSourcePath.clear();
    load.previousSourceUuid.clear();
    load.previousDisplayName.clear();
    load.progress = std::make_shared<std::atomic<float>>(0.0f);

    load.future = std::async(std::launch::async, [p = modelPath.string(), progress = load.progress]()
    {
        auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(p, [&progress](float fraction)
        {
            progress->store(fraction);
        }));
        ModelCache::Store(p, std::move(data));
        progress->store(1.0f);
    });

    LOG_INFO("Started async load for: {}", modelPath.string());
}

void UpdateViewportModelPreview(RendererSharedState& state, const std::string& requestedModelPath, const glm::vec3& worldPosition)
{
    const std::filesystem::path modelPath = NormalizePath(requestedModelPath);
    if (!std::filesystem::exists(modelPath))
    {
        throw std::runtime_error("Preview model asset does not exist: " + modelPath.string());
    }
    if (!ModelLoader::IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error("Preview asset is not a supported glTF model: " + modelPath.string());
    }

    ViewportDragPreviewState& preview = state.viewportDragPreview;
    const bool previewEntityStillExists =
        preview.active &&
        std::find(
            state.GetEditorWorld().GetEntityOrder().begin(),
            state.GetEditorWorld().GetEntityOrder().end(),
            preview.entity
        ) != state.GetEditorWorld().GetEntityOrder().end();

    if (!previewEntityStillExists || preview.modelPath != modelPath.string())
    {
        ClearViewportModelPreview(state);

        // Hover fires every frame the drag stays over the viewport, well before the user
        // commits to dropping. Only materialize a live preview once the model is already
        // parsed and cached (cheap, synchronous): otherwise RebuildSceneRenderables() below
        // would call ModelLoader::LoadModel() synchronously on the UI thread for a model that
        // may be huge (e.g. NewSponza), blocking the app and spiking memory for the whole
        // parse just from hovering. The actual drop still works correctly via
        // CommitViewportModelPreview -> PlaceModelIntoScene, which uses the async loader.
        if (!ModelCache::IsCached(modelPath.string()))
        {
            return;
        }

        preview.previousSelection =
            state.GetEditorWorld().HasSelection() ? state.GetEditorWorld().GetSelectedEntity() : entt::null;
        preview.modelPath = modelPath.string();

        SerializedEntityData entityData{};
        entityData.tagName = modelPath.stem().string().empty() ? "Model" : modelPath.stem().string();
        entityData.modelDisplayName = modelPath.filename().string();
        entityData.modelSourcePath = modelPath.string();
        entityData.modelSourceUuid = AssetRegistry::GetOrCreateUuid(modelPath);
        entityData.transform.translation = worldPosition;

        const entt::entity previewEntity = state.GetEditorWorld().CreateEntity(entityData);

        try
        {
            RebuildSceneRenderables(state);
        }
        catch (...)
        {
            state.GetEditorWorld().DestroyEntity(previewEntity);
            state.GetEditorWorld().SetSelectedEntity(preview.previousSelection);
            preview = {};
            throw;
        }

        preview.active = true;
        preview.entity = previewEntity;
        if (preview.previousSelection != entt::null)
        {
            state.GetEditorWorld().SetSelectedEntity(preview.previousSelection);
        }
        else
        {
            state.GetEditorWorld().ClearSelection();
        }
        state.lastModelLoadError.clear();
        return;
    }

    state.GetEditorWorld().GetTransform(preview.entity).translation = worldPosition;
}

void CommitViewportModelPreview(RendererSharedState& state, const std::string& requestedModelPath, const glm::vec3& worldPosition)
{
    ViewportDragPreviewState& preview = state.viewportDragPreview;
    const std::filesystem::path modelPath = NormalizePath(requestedModelPath);
    const bool previewEntityStillExists =
        preview.active &&
        preview.modelPath == modelPath.string() &&
        std::find(
            state.GetEditorWorld().GetEntityOrder().begin(),
            state.GetEditorWorld().GetEntityOrder().end(),
            preview.entity
        ) != state.GetEditorWorld().GetEntityOrder().end();

    if (!previewEntityStillExists)
    {
        PlaceModelIntoScene(state, modelPath.string(), worldPosition);
        return;
    }

    state.GetEditorWorld().GetTransform(preview.entity).translation = worldPosition;
    state.GetEditorWorld().SetSelectedEntity(preview.entity);
    preview = {};
    state.lastModelLoadError.clear();
    LOG_INFO(
        "Placed model asset into scene at ({:.3f}, {:.3f}, {:.3f}): {}",
        worldPosition.x,
        worldPosition.y,
        worldPosition.z,
        modelPath.string()
    );
}

void ClearViewportModelPreview(RendererSharedState& state, bool restoreSelection)
{
    ViewportDragPreviewState preview = state.viewportDragPreview;
    if (!preview.active)
    {
        return;
    }

    state.viewportDragPreview = {};
    state.GetEditorWorld().DestroyEntity(preview.entity);
    if (restoreSelection)
    {
        if (preview.previousSelection != entt::null)
        {
            state.GetEditorWorld().SetSelectedEntity(preview.previousSelection);
        }
        else
        {
            state.GetEditorWorld().ClearSelection();
        }
    }

    RebuildSceneRenderables(state);
}

void CreateSceneEntity(RendererSharedState& state)
{
    SerializedEntityData entityData{};
    entityData.tagName = "Entity " + std::to_string(state.GetEditorWorld().GetEntityOrder().size() + 1);
    entityData.modelDisplayName = entityData.tagName;
    const entt::entity entity = state.GetEditorWorld().CreateEntity(entityData);
    state.GetEditorWorld().SetSelectedEntity(entity);
    RebuildSceneRenderables(state);
    state.lastModelLoadError.clear();
    LOG_INFO("Created scene entity '{}'", entityData.tagName);
}

void DeleteSelectedSceneEntity(RendererSharedState& state)
{
    if (!state.GetEditorWorld().HasSelection())
    {
        throw std::runtime_error("No selected scene entity to delete");
    }

    const std::string tagName = state.GetEditorWorld().GetSelectedTag().name;
    state.GetEditorWorld().DestroyEntity(state.GetEditorWorld().GetSelectedEntity());
    RebuildSceneRenderables(state);
    state.lastModelLoadError.clear();
    LOG_INFO("Deleted scene entity '{}'", tagName);
}

void CreateSceneLightEntity(RendererSharedState& state, const std::string& name, LightType type)
{
    SerializedLightData lightData{};
    lightData.lightType = type;
    lightData.tagName = name;
    lightData.transform.translation = state.camera.position + state.camera.GetForward() * 5.0f;
    const entt::entity entity = state.GetEditorWorld().CreateLightEntity(lightData);
    state.GetEditorWorld().SetSelectedEntity(entity);
    LOG_INFO("Created light entity '{}'", lightData.tagName);
}

void DeleteSelectedLightEntity(RendererSharedState& state)
{
    if (!state.GetEditorWorld().HasSelection())
    {
        throw std::runtime_error("No selected entity to delete");
    }

    const entt::entity selected = state.GetEditorWorld().GetSelectedEntity();
    if (!state.GetEditorWorld().HasLightComponent(selected))
    {
        throw std::runtime_error("Selected entity is not a light");
    }

    const std::string tagName = state.GetEditorWorld().GetTag(selected).name;
    state.GetEditorWorld().DestroyLightEntity(selected);
    LOG_INFO("Deleted light entity '{}'", tagName);
}

void ApplySelectedModelBaseColorTexture(RendererSharedState& state, const std::string& path)
{
    if (!state.GetEditorWorld().HasSelection() ||
        state.GetEditorWorld().HasLightComponent(state.GetEditorWorld().GetSelectedEntity()))
    {
        throw std::runtime_error("No selected model entity available to receive the texture");
    }

    entt::entity selectedEntity = state.GetEditorWorld().GetSelectedEntity();
    ModelComponent previousModel = state.GetEditorWorld().GetModel(selectedEntity);
    ModelComponent& model = state.GetEditorWorld().GetModel(selectedEntity);
    if (model.sourcePath.empty())
    {
        throw std::runtime_error("The selected entity does not reference an imported model");
    }

    model.baseColorTextureOverridePath = path;
    model.baseColorTextureOverrideUuid = AssetRegistry::GetOrCreateUuid(path);

    try
    {
        RebuildSceneRenderables(state);
    }
    catch (...)
    {
        state.GetEditorWorld().GetModel(selectedEntity) = previousModel;
        throw;
    }

    state.lastModelLoadError.clear();
    LOG_INFO(
        "Applied selected texture override to '{}': {}",
        state.GetEditorWorld().GetTag(selectedEntity).name,
        path
    );
}

void ClearSelectedModelBaseColorTexture(RendererSharedState& state)
{
    if (!state.GetEditorWorld().HasSelection() ||
        state.GetEditorWorld().HasLightComponent(state.GetEditorWorld().GetSelectedEntity()))
    {
        throw std::runtime_error("No selected model entity available to clear the texture override");
    }

    entt::entity selectedEntity = state.GetEditorWorld().GetSelectedEntity();
    ModelComponent previousModel = state.GetEditorWorld().GetModel(selectedEntity);
    ModelComponent& model = state.GetEditorWorld().GetModel(selectedEntity);
    if (model.sourcePath.empty())
    {
        throw std::runtime_error("The selected entity does not reference an imported model");
    }

    model.baseColorTextureOverridePath.clear();
    model.baseColorTextureOverrideUuid.clear();

    try
    {
        RebuildSceneRenderables(state);
    }
    catch (...)
    {
        state.GetEditorWorld().GetModel(selectedEntity) = previousModel;
        throw;
    }

    state.lastModelLoadError.clear();
    LOG_INFO("Cleared selected texture override for '{}'", state.GetEditorWorld().GetTag(selectedEntity).name);
}

bool PumpAsyncModelLoad(RendererSharedState& state)
{
    AsyncModelLoad& load = state.asyncLoad;
    if (!load.future.valid() ||
        load.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }

    IEditorWorld& world = state.GetEditorWorld();
    const auto IsValid = [&world](entt::entity e) -> bool
    {
        if (e == entt::null)
        {
            return false;
        }
        const auto& order = world.GetEntityOrder();
        return std::find(order.begin(), order.end(), e) != order.end();
    };

    bool renderablesDirty = false;
    try
    {
        load.future.get(); // re-throws if the background parse failed

        RebuildSceneRenderables(state);

        if (IsValid(load.trackedEntity))
        {
            const ModelComponent& model = world.GetModel(load.trackedEntity);
            if (model.hasBounds)
            {
                state.camera.FrameBounds(model.minBounds, model.maxBounds);
            }
            if (load.resetTransformOnComplete)
            {
                world.SetSelectedEntity(load.trackedEntity);
                world.ResetSelectedTransform();
            }
        }

        state.lastModelLoadError.clear();
        renderablesDirty = true;
        LOG_INFO("Async model load complete: {}", load.path);
    }
    catch (const std::exception& error)
    {
        if (IsValid(load.trackedEntity))
        {
            if (load.isReplacement)
            {
                world.GetModel(load.trackedEntity).sourcePath = load.previousSourcePath;
                world.GetModel(load.trackedEntity).sourceUuid = load.previousSourceUuid;
                world.GetModel(load.trackedEntity).displayName = load.previousDisplayName;
            }
            else
            {
                world.DestroyEntity(load.trackedEntity);
                if (IsValid(load.previousSelection))
                {
                    world.SetSelectedEntity(load.previousSelection);
                }
            }
        }

        state.lastModelLoadError = error.what();
        LOG_ERROR("Async model load failed for '{}': {}", load.path, error.what());
    }

    load.future = std::future<void>{}; // consume / reset
    state.lastFrameTime = std::chrono::steady_clock::now();
    return renderablesDirty;
}
}
