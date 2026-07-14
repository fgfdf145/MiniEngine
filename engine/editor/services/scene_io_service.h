#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

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

// Rewrites every scene file under the workspace that references the model.
size_t RefreshReferencedSceneFiles(const std::filesystem::path& modelPath);
}
