#include "capture_state.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <stdexcept>

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
        value = node[key].as<T>();
    }
}

void ReadVec3(const YAML::Node& node, const char* key, glm::vec3& value)
{
    if (node && node[key] && node[key].IsSequence() && node[key].size() == 3)
    {
        value = glm::vec3(node[key][0].as<float>(), node[key][1].as<float>(), node[key][2].as<float>());
    }
}

// Each settings group as a map of its fields, written and read by the same list so the two cannot
// drift apart.
template <typename Visitor>
void VisitRenderDebug(RenderDebugSettings& settings, Visitor&& visit)
{
    visit("", "forward_only", settings.forwardOnly);
    visit("", "clustered_lighting", settings.clusteredLighting);
    visit("", "local_light_shadows", settings.localLightShadows);
    visit("", "taa", settings.taa);
    visit("", "specular_anti_aliasing", settings.specularAntiAliasing);
    visit("", "hdr_output", settings.hdrOutput);
    visit("", "hdr_peak_nits", settings.hdrPeakNits);
    visit("", "khronos_reference", settings.khronosReference);
    visit("", "render_scale", settings.renderScale);
    visit("bloom", "enabled", settings.bloom.enabled);
    visit("bloom", "strength", settings.bloom.strength);
    visit("ssr", "enabled", settings.ssr.enabled);
    visit("ssr", "max_roughness", settings.ssr.maxRoughness);
    visit("ssr", "max_distance", settings.ssr.maxDistance);
    visit("ao", "enabled", settings.ao.enabled);
    visit("ao", "radius", settings.ao.radius);
    visit("ao", "thickness", settings.ao.thickness);
    visit("ao", "slice_count", settings.ao.sliceCount);
    visit("ao", "step_count", settings.ao.stepCount);
    visit("ao", "spatial_filter", settings.ao.spatialFilter);
    visit("ao", "temporal_filter", settings.ao.temporalFilter);
    visit("gi", "enabled", settings.gi.enabled);
    visit("gi", "radius", settings.gi.radius);
    visit("gi", "thickness", settings.gi.thickness);
    visit("gi", "slice_count", settings.gi.sliceCount);
    visit("gi", "step_count", settings.gi.stepCount);
    visit("gi", "strength", settings.gi.strength);
    visit("gi", "spatial_filter", settings.gi.spatialFilter);
    visit("gi", "temporal_filter", settings.gi.temporalFilter);
    visit("ddgi", "enabled", settings.ddgi.enabled);
    visit("ddgi", "levels", settings.ddgi.levels);
    visit("ddgi", "base_spacing", settings.ddgi.baseSpacing);
    visit("ddgi", "probes_per_frame", settings.ddgi.probesPerFrame);
    visit("ddgi", "hysteresis", settings.ddgi.hysteresis);
    visit("ddgi", "normal_bias", settings.ddgi.normalBias);
    visit("ddgi", "view_bias", settings.ddgi.viewBias);
    visit("ddgi", "probe_view_level", settings.ddgi.probeViewLevel);
}

template <typename Visitor>
void VisitExposure(Camera& camera, Visitor&& visit)
{
    AutoExposureSettings& exposure = camera.autoExposure;
    visit("auto_exposure", "enabled", exposure.enabled);
    visit("auto_exposure", "compensation_ev", exposure.compensationEv);
    visit("auto_exposure", "min_ev100", exposure.minEv100);
    visit("auto_exposure", "max_ev100", exposure.maxEv100);
    visit("auto_exposure", "low_percentile", exposure.lowPercentile);
    visit("auto_exposure", "high_percentile", exposure.highPercentile);
    visit("auto_exposure", "adapt_to_brighter_per_second", exposure.adaptToBrighterPerSecond);
    visit("auto_exposure", "adapt_to_darker_per_second", exposure.adaptToDarkerPerSecond);
    visit("auto_exposure", "short_term_range_ev", exposure.shortTermRangeEv);
    visit("auto_exposure", "long_term_to_brighter_per_second", exposure.longTermToBrighterPerSecond);
    visit("auto_exposure", "long_term_to_darker_per_second", exposure.longTermToDarkerPerSecond);
    visit("auto_exposure", "frame_reference_weight", exposure.frameReferenceWeight);
    visit("auto_exposure", "sun_reference_weight", exposure.sunReferenceWeight);
    visit("auto_exposure", "sky_reference_weight", exposure.skyReferenceWeight);
    visit("auto_white_balance", "enabled", camera.autoWhiteBalance.enabled);
    visit("auto_white_balance", "degree", camera.autoWhiteBalance.degree);
    visit("auto_white_balance", "adapt_per_second", camera.autoWhiteBalance.adaptPerSecond);
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
            out << YAML::Key << key << YAML::Value << value;
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
                   VisitExposure(copy.camera, visit);
               });
    out << YAML::EndMap;

    out << YAML::Key << "render_debug" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "gbuffer_view" << YAML::Value << static_cast<uint32_t>(copy.renderDebug.gbufferView);
    EmitGroups(out, [&](auto&& visit)
               {
                   VisitRenderDebug(copy.renderDebug, visit);
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
                   VisitExposure(state.camera, visit);
               });

    const YAML::Node renderDebug = root["render_debug"];
    uint32_t view = 0;
    ReadField(renderDebug, "gbuffer_view", view);
    state.renderDebug.gbufferView = static_cast<GBufferDebugView>(std::min(view, static_cast<uint32_t>(GBufferDebugView::DdgiProbes)));
    ReadGroups(renderDebug, [&](auto&& visit)
               {
                   VisitRenderDebug(state.renderDebug, visit);
               });
    return state;
}
}
