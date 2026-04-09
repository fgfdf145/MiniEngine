#pragma once

#include <imgui.h>
#include <ui/ui_scale.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

struct EditorWindowVisibilitySettings
{
    bool camera = true;
    bool assetManager = true;
    bool inputMonitor = false;
    bool scene = true;
    bool theme = true;
    bool viewport = true;
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
