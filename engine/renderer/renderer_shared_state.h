#pragma once

#include "camera.h"
#include "editor_ui.h"
#include "engine_settings.h"
#include "material.h"
#include "mesh.h"
#include "render_types.h"

#include <editor_scene.h>
#include <input/input.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

struct MaterialTexturePaths
{
    std::string baseColor;
    std::string normal;
    std::string metallic;
    std::string roughness;
    std::string occlusion;
    std::string emissive;
    std::string secondaryBaseColor;
    std::string secondaryNormal;
    std::string secondaryMetallic;
    std::string secondaryRoughness;
    std::string secondaryOcclusion;
    std::string secondaryEmissive;
    std::string blendMask;
};

struct CpuRenderSubmesh
{
    entt::entity entity = entt::null;
    MeshData mesh;
    MaterialPushConstants material;
    MaterialTexturePaths textures;
    bool hasTexCoords = false;
    std::string name;
};

struct ViewportDragPreviewState
{
    bool active = false;
    entt::entity entity = entt::null;
    entt::entity previousSelection = entt::null;
    std::string modelPath;
};

struct RendererSharedState
{
    bool initialized = false;
    bool renderablesDirty = false;
    InputState input;
    Camera camera;
    ViewportMatrices viewportMatrices;
    EditorUiController editorUi;
    EditorScene editorScene;
    std::vector<CpuRenderSubmesh> renderSubmeshes;
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
