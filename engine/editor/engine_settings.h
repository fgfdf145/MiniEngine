#pragma once

#include <imgui.h>
#include <engine/platform/process/process_allocation.h>
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

// Which of the Window menu's panels are open, by the settings key of each (EditorPanel::GetSettingsKey,
// the panel's id; the asset browser keeps its older "asset_manager"). A panel not named keeps its default.
struct EditorWindowVisibilitySettings
{
    std::map<std::string, bool> open;
};

// The colours changed in the Theme window, over the built-in palette (ConfigureImGuiStyle). Only those
// are saved, so a palette change in code still reaches every colour the user left alone. Version 1
// files saved the whole palette; they are not read, as they would pin the palette they were saved with.
inline constexpr int kEditorThemeSettingsVersion = 2;

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

// The Preferences window's Audio section.
struct EngineAudioSettings
{
    bool operator==(const EngineAudioSettings&) const = default;

    // Linear gain over every sound, 0 to 1.
    float masterVolume = 1.0f;
    bool muted = false;

    float EffectiveVolume() const
    {
        return muted ? 0.0f : masterVolume;
    }
};

struct EngineSettings
{
    int version = 1;
    EditorUiSettings editorUi;
    EngineAudioSettings audio;
    // The Preferences window's Process section: the priority class and the CPUs the engine runs on.
    platform::process::ProcessAllocation process;
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
