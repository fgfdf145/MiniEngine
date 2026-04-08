#pragma once

#include "camera.h"
#include "editor_ui.h"
#include "engine_settings.h"
#include "render_types.h"
#include "renderer_world.h"

#include <editor_world.h>
#include <input/input.h>

#include <chrono>
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
