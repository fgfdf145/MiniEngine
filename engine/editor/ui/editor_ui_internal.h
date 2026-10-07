#pragma once

// Internal declarations shared between the editor UI panel translation units.
// Not part of the public engine_editor interface.

#include <engine/platform/file_dialog/file_dialog.h>
#include <engine/scene/scene_components.h>

#include <imgui.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace me
{

class EditorPanel;

// Shared selection highlight color (viewport selection, material graph links).
inline constexpr ImU32 kSelectionOutlineColor = IM_COL32(255, 196, 64, 255);

// --- editor_ui_shared.cpp -------------------------------------------------
bool IsSupportedModelAssetPath(const std::filesystem::path& path);
// A model an import accepts: the loadable formats plus ones it converts (.kn5).
bool IsImportableModelAssetPath(const std::filesystem::path& path);
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

// Asks for a file when `requested` is true (pass the button that asks for one) and returns the
// chosen path in the frame it is chosen. Uses the native dialog; where there is none, or it failed,
// a modal asks for the path to be typed instead. The modal lives in the current ID stack, so call
// this every frame from the same place, not only when the button was pressed.
std::optional<std::string> PickFilePath(FileDialogType type, bool requested);

// --- editor_dockspace.cpp -------------------------------------------------
// The dock space over the main viewport's work area, with the default layout when it is empty: each
// panel docked in its default slot. `resetLayout` puts every window back where that layout has it.
//
// With `autoLayout` (Window > Auto Layout), the splits round the viewport panel are moved so its
// picture is the fixed viewport resolution at one display pixel per output pixel, or the largest
// it can be at that aspect when that does not fit; the panels keep the docks they are in and the
// space is taken from them or given to them. It fits again when the resolution, the window or the
// docks change, not every frame, so a splitter dragged afterwards stays where it was put.
struct ViewportAutoLayout
{
    // The viewport panel's ImGui window and the space it had for its picture when it last drew.
    const char* windowName = nullptr;
    ImVec2 panelArea{0.0f, 0.0f};
    // The fixed resolution in points (its pixels over the display's pixels per point).
    ImVec2 imageSize{0.0f, 0.0f};
    float uiScale = 1.0f;
};
// fittedKey: what the layout was last fitted for, kept by the caller across frames (0: not fitted).
ImGuiID DrawEditorDockspace(
    bool resetLayout, std::span<EditorPanel* const> panels, const std::optional<ViewportAutoLayout>& autoLayout, ImGuiID& fittedKey);
}
