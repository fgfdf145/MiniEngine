#pragma once

#include <string>

namespace me
{

struct RendererSharedState;

// Scene file load/save operations.
namespace SceneIoService
{
// Starts a background parse of the scene file and pre-warms the model cache
// for every referenced model. Throws if another load is already in progress.
void StartAsyncSceneLoad(RendererSharedState& state, const std::string& path);

// Applies a finished async scene load, if any. Returns true when the scene
// changed and renderables were rebuilt.
bool PumpAsyncSceneLoad(RendererSharedState& state);

void SaveScene(RendererSharedState& state, const std::string& path);

// Writes the scene as it is to path without making it the open scene's file: a snapshot, such as the
// one a viewport capture keeps beside its image.
void ExportSceneSnapshot(const RendererSharedState& state, const std::string& path);

// File > New Scene: the startup scene's sun and atmosphere and nothing else, not yet saved
// anywhere. Throws while a model or scene is loading.
void NewScene(RendererSharedState& state);

// Scene > Clear Scene: removes every entity; the environment and the scene's file are kept.
// Throws while a model or scene is loading.
void ClearScene(RendererSharedState& state);
}
}
