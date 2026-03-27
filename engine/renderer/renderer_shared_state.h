#pragma once

#include "camera.h"
#include "editor_ui.h"
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
    std::string lastModelLoadError;
    std::string lastSceneIoError;
    std::optional<std::string> pendingModelPath;
    std::optional<std::string> pendingScenePath;
    RenderExtent requestedViewportExtent{};
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
};
