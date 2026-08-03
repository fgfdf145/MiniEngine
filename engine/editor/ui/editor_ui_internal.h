#pragma once

// Internal declarations shared between the editor UI panel translation units.
// Not part of the public engine_editor interface.

#include <scene_components.h>

#include <imgui.h>

#include <filesystem>
#include <string>

// Shared selection highlight color (viewport selection, material graph links).
inline constexpr ImU32 kSelectionOutlineColor = IM_COL32(255, 196, 64, 255);

// --- editor_ui_shared.cpp -------------------------------------------------
bool IsSupportedModelAssetPath(const std::filesystem::path& path);
std::filesystem::path NormalizeFilesystemPath(const std::filesystem::path& path);
bool HasSecondaryMaterialLayer(const MaterialTextureBlendGraph& blendGraph);
void DrawPrimaryMaterialTextureRows(const ModelImportedMaterialInfo& material);
void DrawSecondaryMaterialTextureRows(const MaterialTextureBlendGraph& blendGraph);
const char* GetLightTypeLabel(LightType type);
ImU32 GetLightTypeColor(LightType type);

// --- editor_dock_toolbar.cpp ----------------------------------------------
void DrawTopToolbar(
    bool& showCameraWindow,
    bool& showAssetManagerWindow,
    bool& showInputMonitorWindow,
    bool& showSceneWindow,
    bool& showThemeWindow,
    bool& showViewportWindow,
    float effectiveUiScale);
ImGuiID DrawDockspaceBelowToolbar(float toolbarHeight);
