#pragma once

struct RendererSharedState;

// Converts the editor scene into CPU render submeshes and publishes them to
// the renderer world. Models are resolved through the shared ModelCache.
void RebuildSceneRenderables(RendererSharedState& state);
