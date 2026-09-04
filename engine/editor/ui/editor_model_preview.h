#pragma once

// Software-rasterized model previews used by the model processor window:
// approximate PBR shaded preview, UV layout preview, and the preview cache.

#include <engine/asset/model_loader.h>
#include <engine/scene/scene_components.h>

#include <vector>

namespace me
{

void ResetMaterialShadedPreviewCache(const char* canvasId);

void DrawModelUvPreview(
    const LoadedModelData& loadedModel,
    int& selectedUvSubmeshIndex,
    float uiScale);

void DrawMaterialShadedPreview(
    const LoadedModelData& loadedModel,
    const std::vector<ModelImportedMaterialInfo>& materials,
    int selectedMaterialIndex,
    float& yaw,
    float& pitch,
    float& distance,
    bool& autoFramePending,
    float uiScale,
    const char* canvasId,
    const char* overlayLabel);
}
