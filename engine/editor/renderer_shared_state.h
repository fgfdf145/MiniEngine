#pragma once

#include "editor_ui.h"
#include "engine_settings.h"

#include <camera.h>
#include <render_types.h>
#include <renderer_world.h>

#include <editor_world.h>
#include <input/input.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

struct ViewportDragPreviewState
{
    bool active = false;
    entt::entity entity = entt::null;
    entt::entity previousSelection = entt::null;
    std::string modelPath;
};

// State for a single in-flight async model parse.
// Main thread writes fields before starting; background thread reads them.
struct AsyncModelLoad
{
    // Path being loaded (set before thread starts, read-only in thread).
    std::string path;

    // Context needed to finalize placement / roll back on failure.
    bool isReplacement = false; // true = LoadSelectedModel, false = PlaceModelIntoScene
    glm::vec3 worldPosition{0.0f};
    entt::entity trackedEntity = entt::null;
    entt::entity previousSelection = entt::null;
    bool resetTransformOnComplete = false;
    std::string previousSourcePath;
    std::string previousSourceUuid;
    std::string previousDisplayName;

    // The async task. Valid while a load is in flight or completed but not yet consumed.
    std::future<void> future;

    // Overall load fraction in [0, 1], written by the loading thread.
    std::shared_ptr<std::atomic<float>> progress;

    bool IsActive() const
    {
        return future.valid();
    }
    bool IsLoading() const
    {
        return future.valid() &&
               future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    }
    float Progress() const
    {
        return progress ? progress->load() : 0.0f;
    }
};

// State for a single in-flight async scene load.
// The background thread parses the scene file and pre-warms the model cache
// for every referenced model; the main thread applies the parsed data once ready.
struct AsyncSceneLoad
{
    std::string path;
    std::future<SerializedSceneData> future;

    // Overall load fraction in [0, 1], written by the loading thread.
    std::shared_ptr<std::atomic<float>> progress;

    bool IsActive() const
    {
        return future.valid();
    }
    bool IsLoading() const
    {
        return future.valid() &&
               future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    }
    float Progress() const
    {
        return progress ? progress->load() : 0.0f;
    }
};

// State for a single in-flight asset import: the file copies run on a
// background thread so large models don't stall the UI frame.
struct AsyncAssetImport
{
    std::string sourcePath;
    std::string destinationDirectory;

    // Resolves to the imported model path; throws on failure.
    std::future<std::string> future;

    bool IsActive() const
    {
        return future.valid();
    }
    bool IsLoading() const
    {
        return future.valid() &&
               future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    }
};

// A model load queued from the UI. Loads are started one at a time as the
// async loader frees up, so batch requests don't get dropped.
struct PendingModelLoad
{
    std::string path;
    // Batch loads always create new entities; single loads keep the historical
    // behavior of replacing the current selection's model when there is one.
    bool placeAsNewEntity = false;
};

struct RendererSharedState
{
    IEditorWorld& GetEditorWorld()
    {
        if (!editorWorld)
        {
            throw std::runtime_error("RendererSharedState editor world has not been created");
        }

        return *editorWorld;
    }

    const IEditorWorld& GetEditorWorld() const
    {
        if (!editorWorld)
        {
            throw std::runtime_error("RendererSharedState editor world has not been created");
        }

        return *editorWorld;
    }

    bool initialized = false;
    bool renderablesDirty = false;
    InputState input;
    Camera camera;
    ViewportMatrices viewportMatrices;
    EditorUiController editorUi;
    std::unique_ptr<IEditorWorld> editorWorld;
    RendererWorld rendererWorld;
    ViewportDragPreviewState viewportDragPreview;
    AsyncModelLoad asyncLoad;
    AsyncSceneLoad asyncSceneLoad;
    AsyncAssetImport asyncImport;
    std::string lastModelLoadError;
    std::string lastSceneIoError;
    std::string lastEngineSettingsError;
    std::deque<PendingModelLoad> pendingModelLoads;
    std::optional<std::string> pendingScenePath;
    std::filesystem::path engineSettingsPath;
    EngineSettings engineSettings;
    bool engineSettingsNeedsBootstrapSave = false;
    RenderExtent requestedViewportExtent{};
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
};
