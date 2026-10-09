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

// The scene as a save would write it, less what is not the user's edit: the selection and the gizmo
// settings, a driven car or a car on the rig away from its start, a seated driver's place (its car's),
// the clock of a running time of day, and a model being dragged in from the asset browser.
std::string SceneFingerprint(RendererSharedState& state);
// The scene as it is now is the saved one (after a load, a save or a new scene).
void MarkSceneSaved(RendererSharedState& state);
// Whether the scene differs from the one last loaded, saved or started; false while a scene loads.
// The first call takes the scene as it is for the saved one.
bool HasUnsavedChanges(RendererSharedState& state);
}
}
