#include "engine_settings.h"

#include <engine/core/file/atomic_file.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/editor/view_settings_fields.h>
#include <engine/scene/world_units.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <type_traits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace me
{

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

// A value the file does not hold, or holds in a form this build does not know, keeps its default.
void LoadProcessSettings(const YAML::Node& node, platform::process::ProcessAllocation& process)
{
    if (!node || !node.IsMap())
    {
        return;
    }
    if (const auto priority = platform::process::ParseProcessPriority(node["priority"].as<std::string>("")))
    {
        process.priority = *priority;
    }
    if (const auto cpus = platform::process::ParseCpuSelection(node["cpus"].as<std::string>("")))
    {
        process.cpus = *cpus;
    }
    if (const YAML::Node custom = node["custom_cpus"]; custom && custom.IsSequence())
    {
        process.customCpus.clear();
        for (const YAML::Node& cpu : custom)
        {
            try
            {
                process.customCpus.push_back(cpu.as<uint32_t>());
            }
            catch (const YAML::Exception&)
            {
            }
        }
        std::sort(process.customCpus.begin(), process.customCpus.end());
        process.customCpus.erase(std::unique(process.customCpus.begin(), process.customCpus.end()), process.customCpus.end());
    }
}

void WriteProcessSettings(std::ostream& output, const platform::process::ProcessAllocation& process)
{
    output << "  \"process\": {\n";
    output << "    \"priority\": \"" << platform::process::ProcessPriorityKey(process.priority) << "\",\n";
    output << "    \"cpus\": \"" << platform::process::CpuSelectionKey(process.cpus) << "\",\n";
    output << "    \"custom_cpus\": [";
    for (size_t index = 0; index < process.customCpus.size(); ++index)
    {
        output << (index > 0 ? ", " : "") << process.customCpus[index];
    }
    output << "]\n";
    output << "  },\n";
}

bool ReadBoolOrDefault(const YAML::Node& node, bool defaultValue)
{
    if (!node || !node.IsScalar())
    {
        return defaultValue;
    }
    return node.as<bool>(defaultValue);
}

int ReadIntOrDefault(const YAML::Node& node, int defaultValue)
{
    if (!node || !node.IsScalar())
    {
        return defaultValue;
    }
    return node.as<int>(defaultValue);
}

glm::vec3 ReadVec3OrDefault(const YAML::Node& node, const glm::vec3& defaultValue)
{
    if (!node || !node.IsSequence() || node.size() != 3)
    {
        return defaultValue;
    }
    return glm::vec3(
        ReadFloatOrDefault(node[0], defaultValue.x),
        ReadFloatOrDefault(node[1], defaultValue.y),
        ReadFloatOrDefault(node[2], defaultValue.z));
}

void LoadQuadRecordingSettings(const YAML::Node& node, QuadRecordingSettings& settings)
{
    if (!node || !node.IsMap())
    {
        return;
    }
    if (const YAML::Node layout = node["layout"]; layout && layout.IsScalar())
    {
        settings.layout = layout.as<std::string>() == "cross" ? VideoMosaicLayout::Cross : VideoMosaicLayout::Grid;
    }
    settings.framesPerSecond = static_cast<uint32_t>(std::max(1, ReadIntOrDefault(node["frames_per_second"], static_cast<int>(settings.framesPerSecond))));
    settings.labels = ReadBoolOrDefault(node["labels"], settings.labels);
    if (const YAML::Node cameras = node["cameras"]; cameras && cameras.IsSequence())
    {
        for (size_t index = 0; index < std::min<size_t>(cameras.size(), kQuadCameraCount); ++index)
        {
            const YAML::Node camera = cameras[index];
            if (!camera.IsMap())
            {
                continue;
            }
            QuadCameraSettings& target = settings.cameras[index];
            target.width = static_cast<uint32_t>(std::max(1, ReadIntOrDefault(camera["width"], static_cast<int>(target.width))));
            target.height = static_cast<uint32_t>(std::max(1, ReadIntOrDefault(camera["height"], static_cast<int>(target.height))));
            target.position = ReadVec3OrDefault(camera["position"], target.position);
            target.target = ReadVec3OrDefault(camera["target"], target.target);
            target.fovDegrees = ReadFloatOrDefault(camera["fov_degrees"], target.fovDegrees);
            target.followBodyTilt = ReadBoolOrDefault(camera["follow_body_tilt"], target.followBodyTilt);
        }
    }
    settings = ClampQuadRecordingSettings(settings);
}

void WriteQuadRecordingSettings(std::ostream& output, const QuadRecordingSettings& settings)
{
    const auto vec3 = [](const glm::vec3& value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << "[" << value.x << ", " << value.y << ", " << value.z << "]";
        return stream.str();
    };
    output << "  \"quad_recording\": {\n";
    output << "    \"layout\": \"" << (settings.layout == VideoMosaicLayout::Cross ? "cross" : "grid") << "\",\n";
    output << "    \"frames_per_second\": " << settings.framesPerSecond << ",\n";
    output << "    \"labels\": " << JsonBool(settings.labels) << ",\n";
    output << "    \"cameras\": [\n";
    for (size_t index = 0; index < settings.cameras.size(); ++index)
    {
        const QuadCameraSettings& camera = settings.cameras[index];
        output << "      {\"width\": " << camera.width << ", \"height\": " << camera.height
               << ", \"position\": " << vec3(camera.position) << ", \"target\": " << vec3(camera.target)
               << ", \"fov_degrees\": " << std::fixed << std::setprecision(3) << camera.fovDegrees
               << ", \"follow_body_tilt\": " << JsonBool(camera.followBodyTilt) << "}"
               << (index + 1 < settings.cameras.size() ? ",\n" : "\n");
    }
    output << "    ]\n";
    output << "  },\n";
}

void LoadPhotoModeSettings(const YAML::Node& node, PhotoModeSettings& settings)
{
    if (!node || !node.IsMap())
    {
        return;
    }
    settings.width = static_cast<uint32_t>(std::max(1, ReadIntOrDefault(node["width"], static_cast<int>(settings.width))));
    settings.height = static_cast<uint32_t>(std::max(1, ReadIntOrDefault(node["height"], static_cast<int>(settings.height))));
    settings.warmupFrames =
        static_cast<uint32_t>(std::max(1, ReadIntOrDefault(node["warmup_frames"], static_cast<int>(settings.warmupFrames))));
    settings.framingGuide = ReadBoolOrDefault(node["framing_guide"], settings.framingGuide);
    settings.offlinePathTracing = ReadBoolOrDefault(node["offline_path_tracing"], settings.offlinePathTracing);
    settings.samplesPerPixel =
        static_cast<uint32_t>(std::max(1, ReadIntOrDefault(node["samples_per_pixel"], static_cast<int>(settings.samplesPerPixel))));
    settings.targetSamples =
        static_cast<uint32_t>(std::max(1, ReadIntOrDefault(node["target_samples"], static_cast<int>(settings.targetSamples))));
    const int dlssMode = ReadIntOrDefault(node["dlss_mode"], static_cast<int>(settings.dlssMode));
    settings.dlssMode = dlssMode >= 0 && dlssMode <= static_cast<int>(DlssMode::UltraPerformance) ? static_cast<DlssMode>(dlssMode) : settings.dlssMode;
    settings.dlssRayReconstruction = ReadBoolOrDefault(node["dlss_ray_reconstruction"], settings.dlssRayReconstruction);
    if (node["folder"] && node["folder"].IsScalar())
    {
        settings.folder = node["folder"].as<std::string>();
    }
    settings = ClampPhotoModeSettings(settings);
}

void WritePhotoModeSettings(std::ostream& output, const PhotoModeSettings& settings)
{
    output << "  \"photo_mode\": {\n";
    output << "    \"width\": " << settings.width << ",\n";
    output << "    \"height\": " << settings.height << ",\n";
    output << "    \"warmup_frames\": " << settings.warmupFrames << ",\n";
    output << "    \"framing_guide\": " << JsonBool(settings.framingGuide) << ",\n";
    output << "    \"offline_path_tracing\": " << JsonBool(settings.offlinePathTracing) << ",\n";
    output << "    \"samples_per_pixel\": " << settings.samplesPerPixel << ",\n";
    output << "    \"target_samples\": " << settings.targetSamples << ",\n";
    output << "    \"dlss_mode\": " << static_cast<int>(settings.dlssMode) << ",\n";
    output << "    \"dlss_ray_reconstruction\": " << JsonBool(settings.dlssRayReconstruction) << ",\n";
    output << "    \"folder\": \"" << EscapeJsonString(settings.folder) << "\"\n";
    output << "  },\n";
}

void LoadOptionalUiScale(const YAML::Node& node, std::optional<float>& value)
{
    if (!node || !node.IsScalar())
    {
        return;
    }

    value = platform::ui::ClampUiScale(node.as<float>(value.value_or(1.0f)));
}

void LoadUiScaleSettings(const YAML::Node& scaleNode, platform::ui::UiScaleConfiguration& scale)
{
    if (!scaleNode)
    {
        return;
    }

    if (scaleNode.IsScalar())
    {
        scale.fallback = platform::ui::ClampUiScale(scaleNode.as<float>(scale.fallback));
        return;
    }

    if (!scaleNode.IsMap())
    {
        return;
    }

    scale.fallback = platform::ui::ClampUiScale(ReadFloatOrDefault(scaleNode["default"], scale.fallback));
    LoadOptionalUiScale(scaleNode["windows"], scale.windows);
    LoadOptionalUiScale(scaleNode["linux"], scale.linux);
    LoadOptionalUiScale(scaleNode["macos"], scale.macos);
}

void LoadWindowVisibilitySettings(const YAML::Node& windowsNode, EditorWindowVisibilitySettings& windows)
{
    if (!windowsNode || !windowsNode.IsMap())
    {
        return;
    }

    for (const auto& entry : windowsNode)
    {
        const std::string key = entry.first.as<std::string>("");
        if (!key.empty() && entry.second.IsScalar())
        {
            windows.open[key] = ReadBoolOrDefault(entry.second, false);
        }
    }
}

void LoadThemeSettings(const YAML::Node& themeNode, EditorThemeSettings& theme)
{
    if (!themeNode || !themeNode.IsMap())
    {
        return;
    }

    if (ReadIntOrDefault(themeNode["version"], 1) < kEditorThemeSettingsVersion)
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
            colorValueNode[3].as<float>(1.0f));
        theme.colorDefined[static_cast<size_t>(colorIndex)] = true;
        foundAnyColor = true;
    }

    theme.hasCustomColors = foundAnyColor;
}

template <typename T>
void ReadValue(const YAML::Node& node, T& value)
{
    if (node && node.IsScalar())
    {
        value = node.as<T>(value);
    }
}

// Calls visitAll with a visitor that reads each (group, key, field) from node, a missing group or
// key keeping the field's value.
template <typename VisitAll>
void ReadFields(const YAML::Node& node, VisitAll&& visitAll)
{
    visitAll(
        [&](const char* group, const char* key, auto& value)
        {
            const YAML::Node groupNode = *group == '\0' ? node : node[group];
            if (groupNode && groupNode.IsMap())
            {
                ReadValue(groupNode[key], value);
            }
        });
}

void LoadViewSettings(const YAML::Node& cameraNode, const YAML::Node& renderNode, EngineViewSettings& view)
{
    if (cameraNode && cameraNode.IsMap())
    {
        ReadValue(cameraNode["fov_degrees"], view.fovDegrees);
        ReadValue(cameraNode["near_plane"], view.nearPlane);
        ReadValue(cameraNode["far_plane"], view.farPlane);
        ReadValue(cameraNode["move_speed"], view.moveSpeed);
        ReadValue(cameraNode["mouse_sensitivity"], view.mouseSensitivity);
        ReadValue(cameraNode["exposure_ev100"], view.exposureEv100);
        ReadFields(cameraNode, [&](auto&& visit)
                   {
                       VisitCameraAdaptationFields(view.autoExposure, view.autoWhiteBalance, visit);
                   });
        // The ranges the Camera panel's fields allow, so a hand-edited file cannot set what the panel cannot.
        view.fovDegrees = std::clamp(view.fovDegrees, WorldUnits::kUiCameraFovMinDegrees, WorldUnits::kUiCameraFovMaxDegrees);
        view.nearPlane = std::clamp(view.nearPlane, WorldUnits::kUiCameraNearMinMeters, WorldUnits::kUiCameraNearMaxMeters);
        view.farPlane = std::clamp(view.farPlane, WorldUnits::kUiCameraFarMinMeters, WorldUnits::kUiCameraFarMaxMeters);
        view.exposureEv100 = std::clamp(view.exposureEv100, kMinExposureEv100, kMaxExposureEv100);
        view.autoWhiteBalance.targetKelvin =
            std::clamp(view.autoWhiteBalance.targetKelvin, kMinWhiteBalanceTargetKelvin, kMaxWhiteBalanceTargetKelvin);
    }

    if (renderNode && renderNode.IsMap())
    {
        uint32_t toneMapper = static_cast<uint32_t>(view.renderDebug.toneMapper);
        ReadValue(renderNode["tone_mapper"], toneMapper);
        view.renderDebug.toneMapper = static_cast<ToneMapper>(std::min(toneMapper, static_cast<uint32_t>(ToneMapper::None)));
        uint32_t dlssMode = static_cast<uint32_t>(view.renderDebug.dlssMode);
        ReadValue(renderNode["dlss_mode"], dlssMode);
        view.renderDebug.dlssMode = static_cast<DlssMode>(std::min(dlssMode, static_cast<uint32_t>(DlssMode::UltraPerformance)));
        uint32_t dlssPreset = static_cast<uint32_t>(view.renderDebug.dlssPreset);
        ReadValue(renderNode["dlss_preset"], dlssPreset);
        view.renderDebug.dlssPreset = static_cast<DlssPreset>(std::min(dlssPreset, static_cast<uint32_t>(DlssPreset::M)));
        ReadFields(renderNode, [&](auto&& visit)
                   {
                       VisitRenderDebugFields(view.renderDebug, visit);
                   });
    }
}

template <typename T>
std::string JsonValue(const T& value)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        return JsonBool(value);
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        // The shortest text that reads back as the same float; JSON has no infinity or NaN.
        if (!std::isfinite(value))
        {
            return "0";
        }
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, result.ptr);
    }
    else
    {
        return std::to_string(value);
    }
}

// Collects (group, key, value) fields and writes them as one JSON object, a group as a nested object
// where its first field was.
class JsonFieldWriter
{
  public:
    template <typename T>
    void operator()(const char* group, const char* key, const T& value)
    {
        auto entry = std::find_if(
            m_groups.begin(),
            m_groups.end(),
            [&](const auto& candidate)
            {
                return candidate.first == group;
            });
        if (entry == m_groups.end())
        {
            m_groups.emplace_back(group, std::vector<std::pair<std::string, std::string>>{});
            entry = std::prev(m_groups.end());
        }
        entry->second.emplace_back(key, JsonValue(value));
    }

    // Writes the object from its opening brace to its closing one, its fields indented by indent.
    void Write(std::ostream& output, int indent) const
    {
        const std::string pad(static_cast<size_t>(indent), ' ');
        const std::string innerPad(static_cast<size_t>(indent + 2), ' ');
        output << "{\n";
        size_t lines = 0;
        for (const auto& [group, fields] : m_groups)
        {
            lines += group.empty() ? fields.size() : 1;
        }
        size_t written = 0;
        auto separator = [&]()
        {
            return ++written < lines ? ",\n" : "\n";
        };
        for (const auto& [group, fields] : m_groups)
        {
            if (group.empty())
            {
                for (const auto& [key, value] : fields)
                {
                    output << pad << "\"" << key << "\": " << value << separator();
                }
                continue;
            }
            output << pad << "\"" << group << "\": {\n";
            for (size_t index = 0; index < fields.size(); ++index)
            {
                output << innerPad << "\"" << fields[index].first << "\": " << fields[index].second
                       << (index + 1 < fields.size() ? ",\n" : "\n");
            }
            output << pad << "}" << separator();
        }
        output << std::string(static_cast<size_t>(std::max(indent - 2, 0)), ' ') << "}";
    }

  private:
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> m_groups;
};

void WriteViewSettings(std::ostream& output, const EngineViewSettings& settings)
{
    EngineViewSettings view = settings;

    JsonFieldWriter camera;
    camera("", "fov_degrees", view.fovDegrees);
    camera("", "near_plane", view.nearPlane);
    camera("", "far_plane", view.farPlane);
    camera("", "move_speed", view.moveSpeed);
    camera("", "mouse_sensitivity", view.mouseSensitivity);
    camera("", "exposure_ev100", view.exposureEv100);
    VisitCameraAdaptationFields(view.autoExposure, view.autoWhiteBalance, camera);
    output << "  \"camera\": ";
    camera.Write(output, 4);
    output << ",\n";

    JsonFieldWriter render;
    render("", "tone_mapper", static_cast<uint32_t>(view.renderDebug.toneMapper));
    render("", "dlss_mode", static_cast<uint32_t>(view.renderDebug.dlssMode));
    render("", "dlss_preset", static_cast<uint32_t>(view.renderDebug.dlssPreset));
    VisitRenderDebugFields(view.renderDebug, render);
    output << "  \"render\": ";
    render.Write(output, 4);
    output << "\n";
}
}

void ApplyEngineViewSettings(const EngineViewSettings& view, Camera& camera, RenderDebugSettings& renderDebug)
{
    camera.fovDegrees = view.fovDegrees;
    camera.nearPlane = view.nearPlane;
    camera.farPlane = view.farPlane;
    camera.moveSpeed = view.moveSpeed;
    camera.mouseSensitivity = view.mouseSensitivity;
    camera.exposureEv100 = view.exposureEv100;
    camera.autoExposure = view.autoExposure;
    camera.autoWhiteBalance = view.autoWhiteBalance;
    const GBufferDebugView gbufferView = renderDebug.gbufferView;
    renderDebug = view.renderDebug;
    renderDebug.gbufferView = gbufferView;
}

bool UpdateEngineViewSettings(EngineViewSettings& view, const Camera& camera, const RenderDebugSettings& renderDebug)
{
    EngineViewSettings current;
    current.fovDegrees = camera.fovDegrees;
    current.nearPlane = camera.nearPlane;
    current.farPlane = camera.farPlane;
    current.moveSpeed = camera.moveSpeed;
    current.mouseSensitivity = camera.mouseSensitivity;
    // Auto exposure rewrites the exposure every frame: keep the manual value it will return to.
    current.exposureEv100 = camera.autoExposure.enabled ? view.exposureEv100 : camera.exposureEv100;
    current.autoExposure = camera.autoExposure;
    current.autoWhiteBalance = camera.autoWhiteBalance;
    current.renderDebug = renderDebug;
    current.renderDebug.gbufferView = GBufferDebugView::Off;
    if (current == view)
    {
        return false;
    }
    view = current;
    return true;
}

std::filesystem::path BuildEngineSettingsPath()
{
    return EnginePaths::ProjectRoot() / "miniengine.settings.json";
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
            LoadUiScaleSettings(uiNode["scale"], settings.editorUi.scale);
            LoadWindowVisibilitySettings(uiNode["windows"], settings.editorUi.windows);
            LoadThemeSettings(uiNode["theme"], settings.editorUi.theme);
            settings.editorUi.autoLayout = ReadBoolOrDefault(uiNode["auto_layout"], settings.editorUi.autoLayout);
        }
        LoadViewSettings(root["camera"], root["render"], settings.view);
        if (const YAML::Node audioNode = root["audio"]; audioNode && audioNode.IsMap())
        {
            settings.audio.masterVolume =
                std::clamp(ReadFloatOrDefault(audioNode["master_volume"], settings.audio.masterVolume), 0.0f, 1.0f);
            settings.audio.muted = ReadBoolOrDefault(audioNode["muted"], settings.audio.muted);
        }
        LoadProcessSettings(root["process"], settings.process);
        LoadQuadRecordingSettings(root["quad_recording"], settings.quadRecording);
        LoadPhotoModeSettings(root["photo_mode"], settings.photoMode);

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

        // Built in memory and written atomically: a failed save keeps the
        // previous settings file intact.
        std::ostringstream output;
        output << "{\n";
        output << "  \"version\": " << settings.version << ",\n";
        output << "  \"ui\": {\n";
        output << "    \"scale\": {\n";
        std::vector<std::pair<std::string_view, float>> configuredScales;
        configuredScales.emplace_back("default", platform::ui::ClampUiScale(settings.editorUi.scale.fallback));
        if (settings.editorUi.scale.windows.has_value())
        {
            configuredScales.emplace_back("windows", platform::ui::ClampUiScale(*settings.editorUi.scale.windows));
        }
        if (settings.editorUi.scale.linux.has_value())
        {
            configuredScales.emplace_back("linux", platform::ui::ClampUiScale(*settings.editorUi.scale.linux));
        }
        if (settings.editorUi.scale.macos.has_value())
        {
            configuredScales.emplace_back("macos", platform::ui::ClampUiScale(*settings.editorUi.scale.macos));
        }

        for (size_t scaleIndex = 0; scaleIndex < configuredScales.size(); ++scaleIndex)
        {
            const auto& [key, value] = configuredScales[scaleIndex];
            output << "      \"" << key << "\": " << std::fixed << std::setprecision(3) << value;
            output << (scaleIndex + 1 < configuredScales.size() ? ",\n" : "\n");
        }
        output << "    },\n";
        output << "    \"auto_layout\": " << JsonBool(settings.editorUi.autoLayout) << ",\n";
        output << "    \"windows\": {\n";
        size_t windowIndex = 0;
        for (const auto& [key, open] : settings.editorUi.windows.open)
        {
            output << "      \"" << key << "\": " << JsonBool(open);
            output << (++windowIndex < settings.editorUi.windows.open.size() ? ",\n" : "\n");
        }
        output << "    },\n";
        output << "    \"theme\": {\n";
        output << "      \"version\": " << kEditorThemeSettingsVersion << ",\n";
        output << "      \"colors\": {";

        bool firstColor = true;
        for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
        {
            if (!settings.editorUi.theme.colorDefined[static_cast<size_t>(colorIndex)])
            {
                continue;
            }
            const char* colorName = ImGui::GetStyleColorName(static_cast<ImGuiCol>(colorIndex));
            const ImVec4 color = settings.editorUi.theme.colors[static_cast<size_t>(colorIndex)];
            output << (firstColor ? "\n" : ",\n");
            output << "        \"" << EscapeJsonString(colorName) << "\": ["
                   << std::fixed << std::setprecision(3)
                   << color.x << ", "
                   << color.y << ", "
                   << color.z << ", "
                   << color.w << "]";
            firstColor = false;
        }

        output << (firstColor ? "}\n" : "\n      }\n");
        output << "    }\n";
        output << "  },\n";
        output << "  \"audio\": {\n";
        output << "    \"master_volume\": " << std::fixed << std::setprecision(3) << settings.audio.masterVolume << ",\n";
        output << "    \"muted\": " << JsonBool(settings.audio.muted) << "\n";
        output << "  },\n";
        WriteProcessSettings(output, settings.process);
        WriteQuadRecordingSettings(output, settings.quadRecording);
        WritePhotoModeSettings(output, settings.photoMode);
        WriteViewSettings(output, settings.view);
        output << "}\n";

        std::string writeError;
        if (!AtomicFile::Write(path, output.str(), &writeError))
        {
            throw std::runtime_error("Failed to save engine settings: " + writeError);
        }

        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
        return false;
    }
}
}
