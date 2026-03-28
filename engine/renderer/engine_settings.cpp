#include "engine_settings.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace
{
std::string EscapeJsonString(std::string_view value)
{
    std::ostringstream stream;
    for (const char character : value)
    {
        switch (character)
        {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20u)
            {
                stream << "\\u"
                       << std::hex
                       << std::uppercase
                       << std::setw(4)
                       << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(character))
                       << std::dec
                       << std::nouppercase;
            }
            else
            {
                stream << character;
            }
            break;
        }
    }
    return stream.str();
}

std::string JsonBool(bool value)
{
    return value ? "true" : "false";
}

float ReadFloatOrDefault(const YAML::Node& node, float defaultValue)
{
    if (!node || !node.IsScalar())
    {
        return defaultValue;
    }
    return node.as<float>(defaultValue);
}

bool ReadBoolOrDefault(const YAML::Node& node, bool defaultValue)
{
    if (!node || !node.IsScalar())
    {
        return defaultValue;
    }
    return node.as<bool>(defaultValue);
}

void LoadWindowVisibilitySettings(const YAML::Node& windowsNode, EditorWindowVisibilitySettings& windows)
{
    if (!windowsNode || !windowsNode.IsMap())
    {
        return;
    }

    windows.camera = ReadBoolOrDefault(windowsNode["camera"], windows.camera);
    windows.assetManager = ReadBoolOrDefault(windowsNode["asset_manager"], windows.assetManager);
    windows.scene = ReadBoolOrDefault(windowsNode["scene"], windows.scene);
    windows.theme = ReadBoolOrDefault(windowsNode["theme"], windows.theme);
    windows.viewport = ReadBoolOrDefault(windowsNode["viewport"], windows.viewport);
}

void LoadThemeSettings(const YAML::Node& themeNode, EditorThemeSettings& theme)
{
    if (!themeNode || !themeNode.IsMap())
    {
        return;
    }

    const YAML::Node colorsNode = themeNode["colors"];
    if (!colorsNode || !colorsNode.IsMap())
    {
        return;
    }

    bool foundAnyColor = false;
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        const char* colorName = ImGui::GetStyleColorName(static_cast<ImGuiCol>(colorIndex));
        const YAML::Node colorValueNode = colorsNode[colorName];
        if (!colorValueNode || !colorValueNode.IsSequence() || colorValueNode.size() != 4)
        {
            continue;
        }

        theme.colors[static_cast<size_t>(colorIndex)] = ImVec4(
            colorValueNode[0].as<float>(0.0f),
            colorValueNode[1].as<float>(0.0f),
            colorValueNode[2].as<float>(0.0f),
            colorValueNode[3].as<float>(1.0f)
        );
        theme.colorDefined[static_cast<size_t>(colorIndex)] = true;
        foundAnyColor = true;
    }

    theme.hasCustomColors = foundAnyColor;
}
}

std::filesystem::path BuildEngineSettingsPath()
{
    return std::filesystem::path(MINIENGINE_PROJECT_DIR) / "miniengine.settings.json";
}

bool LoadEngineSettings(const std::filesystem::path& path, EngineSettings& settings, std::string& errorMessage)
{
    errorMessage.clear();
    settings = EngineSettings{};

    if (!std::filesystem::exists(path))
    {
        return true;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(path.string());
        if (!root || !root.IsMap())
        {
            throw std::runtime_error("Engine settings root must be a JSON object");
        }

        settings.version = root["version"].as<int>(settings.version);

        const YAML::Node uiNode = root["ui"];
        if (uiNode && uiNode.IsMap())
        {
            settings.editorUi.scale = std::clamp(ReadFloatOrDefault(uiNode["scale"], settings.editorUi.scale), 0.75f, 3.0f);
            LoadWindowVisibilitySettings(uiNode["windows"], settings.editorUi.windows);
            LoadThemeSettings(uiNode["theme"], settings.editorUi.theme);
        }

        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
        settings = EngineSettings{};
        return false;
    }
}

bool SaveEngineSettings(const std::filesystem::path& path, const EngineSettings& settings, std::string& errorMessage)
{
    errorMessage.clear();

    try
    {
        std::filesystem::create_directories(path.parent_path());

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("Failed to open engine settings file for writing");
        }

        output << "{\n";
        output << "  \"version\": " << settings.version << ",\n";
        output << "  \"ui\": {\n";
        output << "    \"scale\": " << std::fixed << std::setprecision(3) << settings.editorUi.scale << ",\n";
        output << "    \"windows\": {\n";
        output << "      \"camera\": " << JsonBool(settings.editorUi.windows.camera) << ",\n";
        output << "      \"asset_manager\": " << JsonBool(settings.editorUi.windows.assetManager) << ",\n";
        output << "      \"scene\": " << JsonBool(settings.editorUi.windows.scene) << ",\n";
        output << "      \"theme\": " << JsonBool(settings.editorUi.windows.theme) << ",\n";
        output << "      \"viewport\": " << JsonBool(settings.editorUi.windows.viewport) << "\n";
        output << "    },\n";
        output << "    \"theme\": {\n";
        output << "      \"colors\": {\n";

        for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
        {
            const char* colorName = ImGui::GetStyleColorName(static_cast<ImGuiCol>(colorIndex));
            const ImVec4 color = settings.editorUi.theme.colors[static_cast<size_t>(colorIndex)];
            output << "        \"" << EscapeJsonString(colorName) << "\": ["
                   << std::fixed << std::setprecision(3)
                   << color.x << ", "
                   << color.y << ", "
                   << color.z << ", "
                   << color.w << "]";
            output << (colorIndex + 1 < ImGuiCol_COUNT ? ",\n" : "\n");
        }

        output << "      }\n";
        output << "    }\n";
        output << "  }\n";
        output << "}\n";

        if (!output.good())
        {
            throw std::runtime_error("Failed to flush engine settings file");
        }

        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
        return false;
    }
}
