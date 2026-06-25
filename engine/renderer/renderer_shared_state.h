#pragma once

#include "camera.h"
#include "editor_ui.h"
#include "engine_settings.h"
#include "render_types.h"
#include "renderer_world.h"

#include <editor_world.h>
#include <input/input.h>

#include <chrono>
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
    bool isReplacement = false;          // true = LoadSelectedModel, false = PlaceModelIntoScene
    glm::vec3 worldPosition{0.0f};
    entt::entity trackedEntity = entt::null;
    entt::entity previousSelection = entt::null;
    bool resetTransformOnComplete = false;
    std::string previousSourcePath;
    std::string previousDisplayName;

    // The async task. Valid while a load is in flight or completed but not yet consumed.
    std::future<void> future;

    bool IsActive() const { return future.valid(); }
    bool IsLoading() const
    {
        return future.valid() &&
               future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    }
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
    std::string lastModelLoadError;
    std::string lastSceneIoError;
    std::string lastEngineSettingsError;
    std::optional<std::string> pendingModelPath;
    std::optional<std::string> pendingScenePath;
    std::filesystem::path engineSettingsPath;
    EngineSettings engineSettings;
    bool engineSettingsNeedsBootstrapSave = false;
    RenderExtent requestedViewportExtent{};
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
};
