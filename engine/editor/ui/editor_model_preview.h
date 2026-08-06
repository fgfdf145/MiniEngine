#pragma once

// Software-rasterized model previews used by the model processor window:
// approximate PBR shaded preview, UV layout preview, and the preview cache.

#include <model_loader.h>
#include <scene_components.h>

#include <vector>

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
