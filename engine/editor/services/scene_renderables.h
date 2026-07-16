#pragma once

#include <string>

struct RendererSharedState;

// Converts the editor scene into CPU render submeshes and publishes them to
// the renderer world. Models are resolved through the shared ModelCache.
void RebuildSceneRenderables(RendererSharedState& state);
bool RefreshDirtySceneRenderables(RendererSharedState& state);
void MarkModelRenderablesDirtyForSourcePath(RendererSharedState& state, const std::string& sourcePath);
