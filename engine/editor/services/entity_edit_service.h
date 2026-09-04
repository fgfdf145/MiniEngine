#pragma once

#include <engine/scene/scene_components.h>

#include <glm/vec3.hpp>

#include <string>

namespace me
{

struct RendererSharedState;

// Scene entity operations driven by editor UI actions.
namespace EntityEditService
{
void LoadSelectedModel(RendererSharedState& state, const std::string& path, bool resetTransform = true);
void PlaceModelIntoScene(RendererSharedState& state, const std::string& path, const glm::vec3& worldPosition);
void UpdateViewportModelPreview(RendererSharedState& state, const std::string& requestedModelPath, const glm::vec3& worldPosition);
void CommitViewportModelPreview(RendererSharedState& state, const std::string& requestedModelPath, const glm::vec3& worldPosition);
void ClearViewportModelPreview(RendererSharedState& state, bool restoreSelection = true);
void CreateSceneEntity(RendererSharedState& state);
void DeleteSelectedSceneEntity(RendererSharedState& state);
void CreateSceneLightEntity(RendererSharedState& state, const std::string& name, LightType type);
void DeleteSelectedLightEntity(RendererSharedState& state);
void ApplySelectedModelBaseColorTexture(RendererSharedState& state, const std::string& path);
void ClearSelectedModelBaseColorTexture(RendererSharedState& state);

// Applies a finished async model load, if any. Returns true when renderables changed.
bool PumpAsyncModelLoad(RendererSharedState& state);
}
}
