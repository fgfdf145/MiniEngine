#pragma once

#include <engine/asset/model_loader.h>
#include <engine/renderer/renderer_world.h>

#include <glm/glm.hpp>

#include <functional>
#include <string>

namespace me
{

struct RendererSharedState;

// What a submesh draws with, as the scene draws a model's material: the GPU factors and shading
// flags, the texture files (relative paths through resolveTexture), the alpha mode, sides, decal,
// samplers, texture transforms, detail layers and toon material. hasTexCoords: whether the submesh
// has texture coordinates (without them no map is bound); nodeScale: its glTF node's scale, which
// a volume's thickness follows; baseColorOverride replaces the base colour map when not empty. The
// Material Editor's preview fills its submeshes through this too, so it shows what the scene does.
void FillRenderSubmeshMaterial(
    CpuRenderSubmesh& submesh,
    const ModelMaterialData& material,
    bool hasTexCoords,
    const glm::vec3& nodeScale,
    const std::function<std::string(const std::string&)>& resolveTexture,
    const std::string& baseColorOverride = {});

// Converts the editor scene into CPU render submeshes and publishes them to
// the renderer world. Models are resolved through the shared ModelCache.
void RebuildSceneRenderables(RendererSharedState& state);
bool RefreshDirtySceneRenderables(RendererSharedState& state);
void MarkModelRenderablesDirtyForSourcePath(RendererSharedState& state, const std::string& sourcePath);
}
