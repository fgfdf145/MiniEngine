#pragma once

#include <imgui.h>
#include <engine/platform/ui/ui_scale.h>

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

struct EngineSettings
{
    int version = 1;
    EditorUiSettings editorUi;
};

std::filesystem::path BuildEngineSettingsPath();
bool LoadEngineSettings(const std::filesystem::path& path, EngineSettings& settings, std::string& errorMessage);
bool SaveEngineSettings(const std::filesystem::path& path, const EngineSettings& settings, std::string& errorMessage);
}
