#include "entity_edit_service.h"

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
    ModelComponent& selectedModel = state.GetEditorWorld().EditModel(selectedEntity);
    const ModelComponent previousModel = selectedModel;
    selectedModel.sourcePath = path;
    selectedModel.sourceUuid = AssetRegistry::GetOrCreateUuid(path);
    selectedModel.displayName = std::filesystem::path(path).filename().string();

    // Fast path: already cached.
    if (ModelCache::IsCached(path))
    {
        try
        {
            state.GetEditorWorld().MarkModelRenderableDirty(selectedEntity);
            RefreshDirtySceneRenderables(state);
            const ModelBoundsComponent& bounds = state.GetEditorWorld().GetModelBounds(selectedEntity);
            if (bounds.hasBounds)
            {
                state.camera.FrameBounds(bounds.minBounds, bounds.maxBounds);
            }
        }
        catch (...)
        {
            selectedModel = previousModel;
            state.GetEditorWorld().ClearModelRenderableDirty(selectedEntity);
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
            RefreshDirtySceneRenderables(state);
            const ModelBoundsComponent& bounds = state.GetEditorWorld().GetModelBounds(placedEntity);
            if (bounds.hasBounds)
            {
                state.camera.FrameBounds(bounds.minBounds, bounds.maxBounds);
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
        state.GetEditorWorld().HasModelComponent(preview.entity);

    if (!previewEntityStillExists || preview.modelPath != modelPath.string())
    {
        ClearViewportModelPreview(state);

        // Hover fires every frame the drag stays over the viewport, well before the user
        // commits to dropping. Only materialize a live preview once the model is already
        // parsed and cached (cheap, synchronous): otherwise the dirty refresh below
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
            RefreshDirtySceneRenderables(state);
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

    state.GetEditorWorld().EditTransform(preview.entity).translation = worldPosition;
    state.GetEditorWorld().MarkTransformDirty(preview.entity);
}

void CommitViewportModelPreview(RendererSharedState& state, const std::string& requestedModelPath, const glm::vec3& worldPosition)
{
    ViewportDragPreviewState& preview = state.viewportDragPreview;
    const std::filesystem::path modelPath = NormalizePath(requestedModelPath);
    const bool previewEntityStillExists =
        preview.active &&
        preview.modelPath == modelPath.string() &&
        state.GetEditorWorld().HasModelComponent(preview.entity);

    if (!previewEntityStillExists)
    {
        PlaceModelIntoScene(state, modelPath.string(), worldPosition);
        return;
    }

    state.GetEditorWorld().EditTransform(preview.entity).translation = worldPosition;
    state.GetEditorWorld().MarkTransformDirty(preview.entity);
    state.GetEditorWorld().SetSelectedEntity(preview.entity);
    preview = {};
    state.lastModelLoadError.clear();
    LOG_INFO(
        "Placed model asset into scene at ({:.3f}, {:.3f}, {:.3f}): {}",
        worldPosition.x,
        worldPosition.y,
        worldPosition.z,
        modelPath.string());
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

    RefreshDirtySceneRenderables(state);
}

void CreateSceneEntity(RendererSharedState& state)
{
    SerializedEntityData entityData{};
    const size_t modelCount = state.GetEditorWorld().Registry().view<const ModelComponent>().size();
    entityData.tagName = "Entity " + std::to_string(modelCount + 1);
    entityData.modelDisplayName = entityData.tagName;
    const entt::entity entity = state.GetEditorWorld().CreateEntity(entityData);
    state.GetEditorWorld().SetSelectedEntity(entity);
    RefreshDirtySceneRenderables(state);
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
    RefreshDirtySceneRenderables(state);
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
    state.GetEditorWorld().DestroyEntity(selected);
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
    ModelComponent& model = state.GetEditorWorld().EditModel(selectedEntity);
    const ModelComponent previousModel = model;
    if (model.sourcePath.empty())
    {
        throw std::runtime_error("The selected entity does not reference an imported model");
    }

    model.baseColorTextureOverridePath = path;
    model.baseColorTextureOverrideUuid = AssetRegistry::GetOrCreateUuid(path);

    try
    {
        state.GetEditorWorld().MarkModelRenderableDirty(selectedEntity);
        RefreshDirtySceneRenderables(state);
    }
    catch (...)
    {
        model = previousModel;
        state.GetEditorWorld().ClearModelRenderableDirty(selectedEntity);
        throw;
    }

    state.lastModelLoadError.clear();
    LOG_INFO(
        "Applied selected texture override to '{}': {}",
        state.GetEditorWorld().GetTag(selectedEntity).name,
        path);
}

void ClearSelectedModelBaseColorTexture(RendererSharedState& state)
{
    if (!state.GetEditorWorld().HasSelection() ||
        state.GetEditorWorld().HasLightComponent(state.GetEditorWorld().GetSelectedEntity()))
    {
        throw std::runtime_error("No selected model entity available to clear the texture override");
    }

    entt::entity selectedEntity = state.GetEditorWorld().GetSelectedEntity();
    ModelComponent& model = state.GetEditorWorld().EditModel(selectedEntity);
    const ModelComponent previousModel = model;
    if (model.sourcePath.empty())
    {
        throw std::runtime_error("The selected entity does not reference an imported model");
    }

    model.baseColorTextureOverridePath.clear();
    model.baseColorTextureOverrideUuid.clear();

    try
    {
        state.GetEditorWorld().MarkModelRenderableDirty(selectedEntity);
        RefreshDirtySceneRenderables(state);
    }
    catch (...)
    {
        model = previousModel;
        state.GetEditorWorld().ClearModelRenderableDirty(selectedEntity);
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
    const auto IsValidSceneEntity = [&world](entt::entity e) -> bool
    {
        return world.IsValidEntity(e) &&
               world.Registry().all_of<TagComponent, TransformComponent>(e);
    };
    const auto IsValidModelEntity = [&world](entt::entity e) -> bool
    {
        return world.HasModelComponent(e);
    };

    bool renderablesDirty = false;
    try
    {
        load.future.get(); // re-throws if the background parse failed

        if (IsValidModelEntity(load.trackedEntity))
        {
            world.MarkModelRenderableDirty(load.trackedEntity);
        }
        RefreshDirtySceneRenderables(state);

        if (IsValidModelEntity(load.trackedEntity))
        {
            const ModelBoundsComponent& bounds = world.GetModelBounds(load.trackedEntity);
            if (bounds.hasBounds)
            {
                state.camera.FrameBounds(bounds.minBounds, bounds.maxBounds);
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
        if (IsValidModelEntity(load.trackedEntity))
        {
            if (load.isReplacement)
            {
                ModelComponent& model = world.EditModel(load.trackedEntity);
                model.sourcePath = load.previousSourcePath;
                model.sourceUuid = load.previousSourceUuid;
                model.displayName = load.previousDisplayName;
                world.ClearModelRenderableDirty(load.trackedEntity);
            }
            else
            {
                world.DestroyEntity(load.trackedEntity);
                if (IsValidSceneEntity(load.previousSelection))
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
}
