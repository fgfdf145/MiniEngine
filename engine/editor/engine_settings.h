#pragma once

#include <imgui.h>
#include <engine/platform/ui/ui_scale.h>
#include <engine/renderer/camera.h>
#include <engine/renderer/render_types.h>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace me
{

// Which of the Window menu's windows are open, by the settings key of each (EditorUiController's panel
// ids; the asset browser keeps its older "asset_manager"). A window not named keeps its default.
struct EditorWindowVisibilitySettings
{
    std::map<std::string, bool> open;
};

struct EditorThemeSettings
{
    bool hasCustomColors = false;
    std::array<ImVec4, ImGuiCol_COUNT> colors{};
    std::array<bool, ImGuiCol_COUNT> colorDefined{};
};

struct EditorUiSettings
{
    platform::ui::UiScaleConfiguration scale = platform::ui::BuildDefaultUiScaleConfiguration();
    EditorWindowVisibilitySettings windows;
    EditorThemeSettings theme;
};

// The camera's settings and the renderer's: what the Camera and Graphics Debug panels set, kept from
// one run to the next. Not where the camera is (the scene decides that) nor the G-buffer debug view.
struct EngineViewSettings
{
    bool operator==(const EngineViewSettings&) const = default;

    float fovDegrees = Camera{}.fovDegrees;
    float nearPlane = Camera{}.nearPlane;
    float farPlane = Camera{}.farPlane;
    float moveSpeed = Camera{}.moveSpeed;
    float mouseSensitivity = Camera{}.mouseSensitivity;
    // The manual exposure; while auto exposure is on, the renderer's adapted value is not saved.
    float exposureEv100 = Camera{}.exposureEv100;
    AutoExposureSettings autoExposure;
    AutoWhiteBalanceSettings autoWhiteBalance;
    // The G-buffer view in it is always Off.
    RenderDebugSettings renderDebug;
};

struct EngineSettings
{
    int version = 1;
    EditorUiSettings editorUi;
    EngineViewSettings view;
};

// Sets the camera's and the renderer's settings from the saved ones; the G-buffer view is left as it is.
void ApplyEngineViewSettings(const EngineViewSettings& view, Camera& camera, RenderDebugSettings& renderDebug);

// Copies the current camera and renderer settings into view; true when that changed it.
bool UpdateEngineViewSettings(EngineViewSettings& view, const Camera& camera, const RenderDebugSettings& renderDebug);

std::filesystem::path BuildEngineSettingsPath();
bool LoadEngineSettings(const std::filesystem::path& path, EngineSettings& settings, std::string& errorMessage);
bool SaveEngineSettings(const std::filesystem::path& path, const EngineSettings& settings, std::string& errorMessage);
}
