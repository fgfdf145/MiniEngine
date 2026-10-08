#include "capture_state.h"

#include <engine/editor/view_settings_fields.h>

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace me
{

namespace
{
void EmitVec3(YAML::Emitter& out, const char* key, const glm::vec3& value)
{
    out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << YAML::EndSeq;
}

template <typename T>
void ReadField(const YAML::Node& node, const char* key, T& value)
{
    if (node && node[key])
    {
        if constexpr (std::is_enum_v<T>)
        {
            // Enums are stored as their numbers.
            value = static_cast<T>(node[key].as<std::underlying_type_t<T>>());
        }
        else
        {
            value = node[key].as<T>();
        }
    }
}

void ReadVec3(const YAML::Node& node, const char* key, glm::vec3& value)
{
    if (node && node[key] && node[key].IsSequence() && node[key].size() == 3)
    {
        value = glm::vec3(node[key][0].as<float>(), node[key][1].as<float>(), node[key][2].as<float>());
    }
}

// Emits one map per group, the ungrouped fields at the top level of the current map.
template <typename Visit>
void EmitGroups(YAML::Emitter& out, Visit&& visitAll)
{
    std::string openGroup;
    visitAll(
        [&](const char* group, const char* key, auto& value)
        {
            if (openGroup != group)
            {
                if (!openGroup.empty())
                {
                    out << YAML::EndMap;
                }
                openGroup = group;
                if (!openGroup.empty())
                {
                    out << YAML::Key << group << YAML::Value << YAML::BeginMap;
                }
            }
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_enum_v<Value>)
            {
                out << YAML::Key << key << YAML::Value << static_cast<std::underlying_type_t<Value>>(value);
            }
            else
            {
                out << YAML::Key << key << YAML::Value << value;
            }
        });
    if (!openGroup.empty())
    {
        out << YAML::EndMap;
    }
}

template <typename Visit>
void ReadGroups(const YAML::Node& node, Visit&& visitAll)
{
    visitAll(
        [&](const char* group, const char* key, auto& value)
        {
            ReadField(std::string(group).empty() ? node : node[group], key, value);
        });
}

std::string ReplayCommand(const std::filesystem::path& statePath)
{
    return "miniengine_app --state " + statePath.filename().string() +
           " --frames 600 --capture replay.png   (run from this folder; --state waits for the scene to load)";
}
}

void CaptureStateService::Write(const std::filesystem::path& path, const CaptureState& state)
{
    CaptureState copy = state;
    YAML::Emitter out;
    out << YAML::Comment("Replay: " + ReplayCommand(path));
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << 1;
    out << YAML::Key << "scene" << YAML::Value << copy.scenePath.generic_string();
    out << YAML::Key << "original_scene" << YAML::Value << copy.originalScenePath;
    out << YAML::Key << "viewport_size" << YAML::Value << YAML::Flow << YAML::BeginSeq << copy.viewportExtent.width
        << copy.viewportExtent.height << YAML::EndSeq;

    out << YAML::Key << "camera" << YAML::Value << YAML::BeginMap;
    EmitVec3(out, "position", copy.camera.position);
    out << YAML::Key << "yaw_degrees" << YAML::Value << copy.camera.yawDegrees;
    out << YAML::Key << "pitch_degrees" << YAML::Value << copy.camera.pitchDegrees;
    out << YAML::Key << "fov_degrees" << YAML::Value << copy.camera.fovDegrees;
    out << YAML::Key << "near_plane" << YAML::Value << copy.camera.nearPlane;
    out << YAML::Key << "far_plane" << YAML::Value << copy.camera.farPlane;
    // The exposure the capture was taken at; auto exposure adapts from scratch on replay.
    out << YAML::Key << "exposure_ev100" << YAML::Value << copy.camera.exposureEv100;
    EmitGroups(out, [&](auto&& visit)
               {
                   VisitCameraAdaptationFields(copy.camera.autoExposure, copy.camera.autoWhiteBalance, visit);
               });
    out << YAML::EndMap;

    out << YAML::Key << "render_debug" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "gbuffer_view" << YAML::Value << static_cast<uint32_t>(copy.renderDebug.gbufferView);
    out << YAML::Key << "tone_mapper" << YAML::Value << static_cast<uint32_t>(copy.renderDebug.toneMapper);
    out << YAML::Key << "dlss_mode" << YAML::Value << static_cast<uint32_t>(copy.renderDebug.dlssMode);
    out << YAML::Key << "dlss_preset" << YAML::Value << static_cast<uint32_t>(copy.renderDebug.dlssPreset);
    EmitGroups(out, [&](auto&& visit)
               {
                   VisitRenderDebugFields(copy.renderDebug, visit);
               });
    out << YAML::EndMap;
    out << YAML::EndMap;

    std::ofstream file(path);
    if (!file)
    {
        throw std::runtime_error("Failed to write '" + path.string() + "'");
    }
    file << out.c_str() << '\n';
}

CaptureState CaptureStateService::Read(const std::filesystem::path& path)
{
    YAML::Node root;
    try
    {
        root = YAML::LoadFile(path.string());
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Failed to read the capture state '" + path.string() + "': " + error.what());
    }
    if (!root["scene"])
    {
        throw std::runtime_error("The capture state '" + path.string() + "' names no scene");
    }
    CaptureState state;
    const std::filesystem::path scene = root["scene"].as<std::string>();
    state.scenePath = scene.is_absolute() ? scene : path.parent_path() / scene;
    ReadField(root, "original_scene", state.originalScenePath);
    if (root["viewport_size"] && root["viewport_size"].size() == 2)
    {
        state.viewportExtent = RenderExtent{root["viewport_size"][0].as<uint32_t>(), root["viewport_size"][1].as<uint32_t>()};
    }

    const YAML::Node camera = root["camera"];
    ReadVec3(camera, "position", state.camera.position);
    ReadField(camera, "yaw_degrees", state.camera.yawDegrees);
    ReadField(camera, "pitch_degrees", state.camera.pitchDegrees);
    ReadField(camera, "fov_degrees", state.camera.fovDegrees);
    ReadField(camera, "near_plane", state.camera.nearPlane);
    ReadField(camera, "far_plane", state.camera.farPlane);
    ReadField(camera, "exposure_ev100", state.camera.exposureEv100);
    ReadGroups(camera, [&](auto&& visit)
               {
                   VisitCameraAdaptationFields(state.camera.autoExposure, state.camera.autoWhiteBalance, visit);
               });

    const YAML::Node renderDebug = root["render_debug"];
    uint32_t view = 0;
    ReadField(renderDebug, "gbuffer_view", view);
    state.renderDebug.gbufferView = static_cast<GBufferDebugView>(std::min(view, static_cast<uint32_t>(kLastGBufferDebugView)));
    uint32_t toneMapper = 0;
    ReadField(renderDebug, "tone_mapper", toneMapper);
    state.renderDebug.toneMapper = static_cast<ToneMapper>(std::min(toneMapper, static_cast<uint32_t>(ToneMapper::None)));
    uint32_t dlssMode = 0;
    ReadField(renderDebug, "dlss_mode", dlssMode);
    state.renderDebug.dlssMode = static_cast<DlssMode>(std::min(dlssMode, static_cast<uint32_t>(DlssMode::UltraPerformance)));
    uint32_t dlssPreset = 0;
    ReadField(renderDebug, "dlss_preset", dlssPreset);
    state.renderDebug.dlssPreset = static_cast<DlssPreset>(std::min(dlssPreset, static_cast<uint32_t>(DlssPreset::M)));
    ReadGroups(renderDebug, [&](auto&& visit)
               {
                   VisitRenderDebugFields(state.renderDebug, visit);
               });
    return state;
}
}
