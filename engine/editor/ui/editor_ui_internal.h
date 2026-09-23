#pragma once

// Internal declarations shared between the editor UI panel translation units.
// Not part of the public engine_editor interface.

#include <engine/scene/scene_components.h>

#include <imgui.h>

#include <filesystem>
#include <string>

namespace me
{

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

// Every numeric field in the editor is a drag field, so they all work the same way: drag to change,
// double-click or Ctrl+click to type. These stand in for ImGui sliders: a speed of 0 crosses the
// range in about a slider's width, and typed values are clamped to the range as a slider's were.
bool DragFloatInRange(
    const char* label,
    float* value,
    float min,
    float max,
    const char* format = "%.3f",
    float speed = 0.0f);
bool DragFloat3InRange(
    const char* label,
    float* values,
    float min,
    float max,
    const char* format = "%.3f",
    float speed = 0.0f);
bool DragIntInRange(const char* label, int* value, int min, int max);

// --- editor_dock_toolbar.cpp ----------------------------------------------
void DrawTopToolbar(
    bool& showCameraWindow,
    bool& showAssetManagerWindow,
    bool& showInputMonitorWindow,
    bool& showSceneWindow,
    bool& showThemeWindow,
    bool& showViewportWindow,
    bool& showGraphicsDebugWindow,
    float effectiveUiScale);
ImGuiID DrawDockspaceBelowToolbar(float toolbarHeight);
}
