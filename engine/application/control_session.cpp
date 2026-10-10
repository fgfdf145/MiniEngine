#include "control_session.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/core/version/engine_version.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/photo_mode.h>
#include <engine/editor/services/scene_io_service.h>
#include <engine/editor/services/vehicle_drive_service.h>
#include <engine/editor/view_settings_fields.h>
#include <engine/platform/crash/crash_handler.h>
#include <engine/platform/renderdoc/renderdoc_capture.h>
#include <engine/logic/editor_world.h>
#include <engine/renderer/rhi/backend.h>

#include <glm/gtc/quaternion.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#include <process.h>
#define ME_GETPID _getpid
#else
#include <unistd.h>
#define ME_GETPID getpid
#endif

namespace me
{

using nlohmann::json;

namespace
{
json ToJson(const glm::vec3& value)
{
    return json::array({value.x, value.y, value.z});
}

json ToJson(const glm::dvec3& value)
{
    return json::array({value.x, value.y, value.z});
}

glm::vec3 ReadVec3(const json& value, const char* name)
{
    if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
    {
        throw std::runtime_error(std::string("'") + name + "' takes [x, y, z]");
    }
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

template <typename T>
T ReadNumber(const json& args, const char* name, T fallback)
{
    if (!args.contains(name) || args[name].is_null())
    {
        return fallback;
    }
    if (!args[name].is_number())
    {
        throw std::runtime_error(std::string("'") + name + "' takes a number");
    }
    return args[name].get<T>();
}

std::string ReadString(const json& args, const char* name)
{
    if (!args.contains(name) || !args[name].is_string())
    {
        throw std::runtime_error(std::string("'") + name + "' (a string) is needed");
    }
    return args[name].get<std::string>();
}

// Sets one settings field from JSON, of the field's own type; throws on another type.
template <typename T>
void AssignField(T& field, const json& value, const std::string& key)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        if (!value.is_boolean())
        {
            throw std::runtime_error("'" + key + "' takes true or false");
        }
        field = value.get<bool>();
    }
    else if constexpr (std::is_arithmetic_v<T>)
    {
        if (!value.is_number())
        {
            throw std::runtime_error("'" + key + "' takes a number");
        }
        if constexpr (std::is_integral_v<T>)
        {
            const double number = value.get<double>();
            if (number != std::floor(number))
            {
                throw std::runtime_error("'" + key + "' takes a whole number");
            }
            if constexpr (std::is_unsigned_v<T>)
            {
                if (number < 0.0)
                {
                    throw std::runtime_error("'" + key + "' cannot be negative");
                }
            }
        }
        field = value.get<T>();
    }
    else
    {
        throw std::runtime_error("'" + key + "' cannot be set from the control channel");
    }
}

std::string FieldKey(const char* group, const char* key)
{
    return group[0] == '\0' ? std::string(key) : std::string(group) + "." + key;
}

// Every render setting the engine settings file holds, flat ("group.key"), plus the enums that file
// writes as numbers and the camera's adaptation ("camera." first).
template <typename Visit>
void VisitControlFields(RenderDebugSettings& render, Camera& camera, Visit&& visit)
{
    auto toneMapper = static_cast<uint32_t>(render.toneMapper);
    auto dlssMode = static_cast<uint32_t>(render.dlssMode);
    auto dlssPreset = static_cast<uint32_t>(render.dlssPreset);
    auto gbufferView = static_cast<uint32_t>(render.gbufferView);
    visit(std::string("tone_mapper"), toneMapper);
    visit(std::string("dlss_mode"), dlssMode);
    visit(std::string("dlss_preset"), dlssPreset);
    visit(std::string("gbuffer_view"), gbufferView);
    render.toneMapper = static_cast<ToneMapper>(std::min(toneMapper, static_cast<uint32_t>(ToneMapper::None)));
    render.dlssMode = static_cast<DlssMode>(std::min(dlssMode, static_cast<uint32_t>(DlssMode::UltraPerformance)));
    render.dlssPreset = static_cast<DlssPreset>(std::min(dlssPreset, static_cast<uint32_t>(DlssPreset::M)));
    render.gbufferView = static_cast<GBufferDebugView>(gbufferView);
    VisitRenderDebugFields(render, [&](const char* group, const char* key, auto& field)
                           {
                               visit(FieldKey(group, key), field);
                           });
    VisitCameraAdaptationFields(camera.autoExposure, camera.autoWhiteBalance, [&](const char* group, const char* key, auto& field)
                                {
                                    visit("camera." + FieldKey(group, key), field);
                                });
}

template <typename T>
json FieldToJson(const T& field)
{
    if constexpr (std::is_arithmetic_v<T>)
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            if (!std::isfinite(field))
            {
                return nullptr;
            }
        }
        return field;
    }
    else
    {
        return nullptr;
    }
}

json CameraToJson(const Camera& camera)
{
    return {
        {"position", ToJson(camera.position)},
        {"yaw", camera.yawDegrees},
        {"pitch", camera.pitchDegrees},
        {"forward", ToJson(camera.GetForward())},
        {"fov", camera.fovDegrees},
        {"near", camera.nearPlane},
        {"far", camera.farPlane},
        {"exposure_ev100", camera.exposureEv100},
        {"auto_exposure", camera.autoExposure.enabled},
        {"adapted_ev100", camera.adaptedLongTermEv100},
        {"auto_white_balance", camera.autoWhiteBalance.enabled}};
}

const char* EntityKind(const IEditorWorld& world, entt::entity entity)
{
    if (world.HasLightComponent(entity))
    {
        return "light";
    }
    if (world.HasModelComponent(entity))
    {
        return "model";
    }
    return "entity";
}

json EntityToJson(const IEditorWorld& world, entt::entity entity)
{
    const TransformComponent& transform = world.GetTransform(entity);
    json result = {
        {"id", static_cast<uint32_t>(entity)},
        {"name", world.GetTag(entity).name},
        {"kind", EntityKind(world, entity)},
        {"position", ToJson(transform.translation)},
        {"rotation", ToJson(transform.rotationDegrees)},
        {"scale", ToJson(transform.scale)}};
    if (world.HasModelComponent(entity))
    {
        result["model"] = world.GetModel(entity).sourcePath;
    }
    return result;
}

// The entity args name: "id" (as entities.list gives it) or "name" (its tag; the first in scene order).
entt::entity FindEntity(const IEditorWorld& world, const json& args)
{
    if (args.contains("id"))
    {
        const auto entity = static_cast<entt::entity>(args["id"].get<uint32_t>());
        if (!world.IsValidEntity(entity))
        {
            throw std::runtime_error("No entity has id " + args["id"].dump());
        }
        return entity;
    }
    const std::string name = ReadString(args, "name");
    for (const entt::entity entity : world.GetSceneOrder())
    {
        if (world.GetTag(entity).name == name)
        {
            return entity;
        }
    }
    throw std::runtime_error("The scene has no entity named '" + name + "' (entities.list lists them)");
}

// A scene file's YAML as JSON: numbers and booleans as such, the rest strings.
json YamlToJson(const YAML::Node& node)
{
    switch (node.Type())
    {
    case YAML::NodeType::Map:
    {
        json result = json::object();
        for (const auto& entry : node)
        {
            result[entry.first.as<std::string>()] = YamlToJson(entry.second);
        }
        return result;
    }
    case YAML::NodeType::Sequence:
    {
        json result = json::array();
        for (const auto& entry : node)
        {
            result.push_back(YamlToJson(entry));
        }
        return result;
    }
    case YAML::NodeType::Scalar:
    {
        const std::string& text = node.Scalar();
        if (node.Tag() == "!")
        {
            // Quoted in the file: a string whatever it looks like.
            return text;
        }
        if (text == "true" || text == "false")
        {
            return text == "true";
        }
        if (text.empty() || !(std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-' || text[0] == '.'))
        {
            return text;
        }
        long long integer = 0;
        if (text.find_first_of(".eE") == std::string::npos && YAML::convert<long long>::decode(node, integer))
        {
            return integer;
        }
        double number = 0.0;
        if (YAML::convert<double>::decode(node, number))
        {
            return number;
        }
        return text;
    }
    default:
        return nullptr;
    }
}

// The scene as its file would hold it, as JSON.
json SceneDocument(const IEditorWorld& world)
{
    return YamlToJson(YAML::Load(SerializeEditorSceneData(world.CaptureSceneData())));
}

std::vector<std::string> SplitPath(const std::string& path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size())
    {
        const size_t dot = path.find('.', start);
        const size_t end = dot == std::string::npos ? path.size() : dot;
        if (end > start)
        {
            parts.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

// Steps one segment into a scene document: a key of an object; in an array an index or the tag of
// the element that has it ("lights.Key box").
json& StepInto(json& node, const std::string& part, const std::string& path)
{
    if (node.is_object())
    {
        const auto found = node.find(part);
        if (found == node.end())
        {
            std::string keys;
            for (const auto& [key, value] : node.items())
            {
                keys += (keys.empty() ? "" : ", ") + key;
            }
            throw std::runtime_error("'" + path + "': no '" + part + "' here (there is " + keys + ")");
        }
        return *found;
    }
    if (node.is_array())
    {
        const bool numeric = std::all_of(part.begin(), part.end(), [](char c)
                                          {
                                              return std::isdigit(static_cast<unsigned char>(c)) != 0;
                                          });
        if (numeric)
        {
            const size_t index = std::stoul(part);
            if (index >= node.size())
            {
                throw std::runtime_error("'" + path + "': index " + part + " is past the " + std::to_string(node.size()) + " elements");
            }
            return node[index];
        }
        for (json& element : node)
        {
            if (element.is_object() && element.value("tag", std::string()) == part)
            {
                return element;
            }
        }
        throw std::runtime_error("'" + path + "': no element tagged '" + part + "'");
    }
    throw std::runtime_error("'" + path + "': '" + part + "' is past a value");
}

// Whether a value may replace the one a scene document holds: a number for a number, an array of as
// many elements of their kinds, and so on.
bool SameKind(const json& current, const json& value)
{
    if (current.is_number())
    {
        return value.is_number();
    }
    if (current.is_array())
    {
        if (!value.is_array() || value.size() != current.size())
        {
            return false;
        }
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (!SameKind(current[index], value[index]))
            {
                return false;
            }
        }
        return true;
    }
    if (current.is_object())
    {
        return value.is_object();
    }
    return current.type() == value.type();
}

// debug.crash throw: an exception out of a noexcept function ends in std::terminate with it current.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4297)
#endif
[[noreturn]] void ThrowOutOfNoexcept(const char* message) noexcept
{
    throw std::runtime_error(message);
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

DlssMode ParseDlssMode(const std::string& value)
{
    static constexpr std::pair<const char*, DlssMode> kModes[] = {
        {"off", DlssMode::Off},
        {"dlaa", DlssMode::Dlaa},
        {"quality", DlssMode::Quality},
        {"balanced", DlssMode::Balanced},
        {"performance", DlssMode::Performance},
        {"ultra-performance", DlssMode::UltraPerformance}};
    for (const auto& [name, mode] : kModes)
    {
        if (value == name)
        {
            return mode;
        }
    }
    throw std::runtime_error("'dlss' takes off, dlaa, quality, balanced, performance or ultra-performance");
}

json TimingsToJson(const IRenderBackend::FrameTimings& timings)
{
    auto list = [](const std::vector<std::pair<std::string, double>>& entries)
    {
        json result = json::array();
        for (const auto& [name, ms] : entries)
        {
            result.push_back({{"name", name}, {"ms", ms}});
        }
        return result;
    };
    return {
        {"frames", timings.frames},
        {"cpu_recording_ms", timings.cpuRecordingMs},
        {"cpu_wait_ms", timings.cpuWaitMs},
        {"gpu_ms", timings.gpuMs},
        {"slowest_cpu_ms", timings.slowestCpuMs},
        {"main_thread_ms", timings.mainThreadMs},
        {"render_thread", timings.renderThread},
        {"cpu_stages", list(timings.cpuStages)},
        {"main_stages", list(timings.mainStages)},
        {"gpu_passes", list(timings.gpuPasses)}};
}

json DriveStatusToJson(const VehicleDriveStatus& status, bool wheels)
{
    json result = {{"active", status.active}, {"paused", status.paused}};
    if (!status.active)
    {
        if (!status.lastRunSummary.empty())
        {
            result["last_run"] = status.lastRunSummary;
        }
        if (!status.lastError.empty())
        {
            result["error"] = status.lastError;
        }
        return result;
    }
    const VehicleTelemetry& telemetry = status.telemetry;
    const glm::vec3 euler = glm::degrees(glm::eulerAngles(status.pose.rotation));
    result.update({
        {"vehicle", status.vehicleName},
        {"position", ToJson(status.pose.position)},
        {"rotation_degrees", ToJson(euler)},
        {"speed_kmh", telemetry.forwardSpeed * 3.6f},
        {"forward_speed", telemetry.forwardSpeed},
        {"right_speed", telemetry.rightSpeed},
        {"engine_rpm", telemetry.engineRpm},
        {"gear", telemetry.gear},
        {"clutch", telemetry.clutch},
        {"turbo_boost", telemetry.turboBoost},
        {"wheels_in_contact", telemetry.wheelsInContact},
        {"abs_active", telemetry.absActive},
        {"traction_control_cut", telemetry.tractionControlCut},
        {"odometer_m", status.odometerMetres},
        {"real_time_share", status.realTimeShare},
        {"manual_gearbox", status.manualGearbox},
        {"controls",
         {{"throttle", status.controls.throttle},
          {"steering", status.controls.steering},
          {"brake", status.controls.brake},
          {"hand_brake", status.controls.handBrake}}},
        {"camera_view", VehicleCameraViewName(status.cameraView)}});
    if (status.automation.mode != VehicleAutomationMode::None)
    {
        result["automation"] = {
            {"name", status.automation.name},
            {"distance", status.automation.distance},
            {"length", status.automation.length},
            {"lap", status.automation.lap},
            {"frame", status.automation.frame},
            {"failure", status.automation.failure}};
    }
    if (wheels)
    {
        static constexpr const char* kCorners[] = {"front_left", "front_right", "rear_left", "rear_right"};
        json list = json::array();
        for (size_t index = 0; index < status.wheels.size(); ++index)
        {
            const VehicleWheelState& wheel = status.wheels[index];
            json entry = {
                {"corner", index < 4 ? kCorners[index] : "extra"},
                {"in_contact", wheel.inContact},
                {"angular_velocity", wheel.angularVelocity},
                {"suspension_length", wheel.suspensionLength},
                {"travel", wheel.travel},
                {"suspension_force", wheel.suspensionForce},
                {"tyre_load", wheel.tyreLoad},
                {"slip_ratio", wheel.slipRatio},
                {"slip_angle_degrees", wheel.slipAngleDegrees},
                {"longitudinal_force", wheel.longitudinalForce},
                {"lateral_force", wheel.lateralForce},
                {"brake_torque", wheel.brakeTorque},
                {"camber_degrees", wheel.camberDegrees},
                {"toe_degrees", wheel.toeDegrees}};
            if (index < telemetry.tyres.size() && telemetry.tyres[index].simulated)
            {
                const VehicleTelemetry::TyreTemperatures& tyre = telemetry.tyres[index];
                entry["tyre"] = {
                    {"tread_c", json::array({tyre.tread[0], tyre.tread[1], tyre.tread[2]})},
                    {"core_c", tyre.core},
                    {"pressure", tyre.pressure},
                    {"grip", tyre.grip}};
            }
            list.push_back(std::move(entry));
        }
        result["wheels"] = std::move(list);
    }
    return result;
}
}

ControlSession::ControlSession(ControlServer& server, RendererSharedState& state, IRenderBackend& renderer)
    : m_server(server),
      m_state(state),
      m_renderer(renderer)
{
    RegisterCommands();
}

void ControlSession::Register(const std::string& name, std::string summary, Handler handler)
{
    m_commands[name] = {std::move(summary), std::move(handler)};
}

void ControlSession::Update(bool frameDrawn)
{
    if (frameDrawn)
    {
        ++m_framesDrawn;
    }
    for (const ControlServer::Request& request : m_server.TakeRequests())
    {
        Handle(request);
    }
    const Clock::time_point now = Clock::now();
    for (auto waiter = m_waiters.begin(); waiter != m_waiters.end();)
    {
        try
        {
            if (std::optional<json> result = waiter->poll(); result.has_value())
            {
                Answer(waiter->connection, waiter->id, *result);
            }
            else if (now < waiter->deadline)
            {
                ++waiter;
                continue;
            }
            else
            {
                Fail(waiter->connection, waiter->id, "'" + waiter->command + "' timed out");
            }
        }
        catch (const std::exception& error)
        {
            Fail(waiter->connection, waiter->id, error.what());
        }
        waiter = m_waiters.erase(waiter);
    }
}

json ControlSession::Execute(const std::string& command, const json& args)
{
    const auto found = m_commands.find(command);
    if (found == m_commands.end())
    {
        throw std::runtime_error("Unknown command '" + command + "' (help lists them)");
    }
    return found->second.handler(args).result;
}

void ControlSession::Handle(const ControlServer::Request& request)
{
    json id = nullptr;
    try
    {
        const json message = json::parse(request.line);
        if (!message.is_object())
        {
            throw std::runtime_error("A request is a JSON object");
        }
        id = message.value("id", json(nullptr));
        const std::string command = message.value("cmd", std::string());
        const json args = message.value("args", json::object());
        if (!args.is_object())
        {
            throw std::runtime_error("'args' is an object");
        }
        const auto found = m_commands.find(command);
        if (found == m_commands.end())
        {
            throw std::runtime_error("Unknown command '" + command + "' (help lists them)");
        }
        Outcome outcome = found->second.handler(args);
        if (outcome.wait)
        {
            const double timeout = outcome.timeoutSeconds > 0.0 ? outcome.timeoutSeconds : 600.0;
            m_waiters.push_back({
                request.connection,
                id,
                command,
                std::move(outcome.wait),
                Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout))});
            return;
        }
        Answer(request.connection, id, outcome.result);
    }
    catch (const std::exception& error)
    {
        Fail(request.connection, id, error.what());
    }
}

void ControlSession::Answer(uint64_t connection, const json& id, const json& result)
{
    m_server.Send(connection, json{{"id", id}, {"ok", true}, {"result", result}}.dump(-1, ' ', false, json::error_handler_t::replace));
}

void ControlSession::Fail(uint64_t connection, const json& id, const std::string& error)
{
    m_server.Send(connection, json{{"id", id}, {"ok", false}, {"error", error}}.dump(-1, ' ', false, json::error_handler_t::replace));
}

void ControlSession::RegisterCommands()
{
    auto now = [](json result)
    {
        Outcome outcome;
        outcome.result = std::move(result);
        return outcome;
    };

    Register("help", "Lists the commands.", [this, now](const json&)
             {
                 json list = json::object();
                 for (const auto& [name, info] : m_commands)
                 {
                     list[name] = info.summary;
                 }
                 return now(list);
             });

    Register("ping", "Engine version, process id, render backend and frames drawn.", [this, now](const json&)
             {
                 return now({
                     {"version", EngineVersion::String()},
                     {"pid", ME_GETPID()},
                     {"backend", ToString(m_renderer.GetBackendType())},
                     {"frame", m_framesDrawn}});
             });

    Register("status", "Frame, scene, loading state, viewport, camera and drive in one answer.", [this, now](const json&)
             {
                 json result = {
                     {"frame", m_framesDrawn},
                     {"scene", m_state.editorWorld ? m_state.GetEditorWorld().GetSceneFilePath() : std::string()},
                     {"loading", m_state.IsSceneLoading()},
                     {"viewport", {m_state.requestedViewportExtent.width, m_state.requestedViewportExtent.height}},
                     {"fixed_viewport", m_state.fixedViewportExtent.has_value()},
                     {"camera", CameraToJson(m_state.camera)},
                     {"taking_photo", m_renderer.IsTakingPhoto()},
                     {"fixed_frame_seconds", m_state.fixedFrameSeconds.has_value() ? json(*m_state.fixedFrameSeconds) : json(nullptr)},
                     {"driving", m_state.vehicleDrive.session != nullptr}};
                 if (!m_state.lastSceneIoError.empty())
                 {
                     result["scene_error"] = m_state.lastSceneIoError;
                 }
                 return now(result);
             });

    Register("frames", "Answers after {count} more frames are drawn (default 1).", [this](const json& args)
             {
                 const auto count = ReadNumber<uint64_t>(args, "count", 1);
                 const uint64_t target = m_framesDrawn + count;
                 Outcome outcome;
                 outcome.timeoutSeconds = ReadNumber<double>(args, "timeout_s", 600.0);
                 outcome.wait = [this, target]() -> std::optional<json>
                 {
                     if (m_framesDrawn < target)
                     {
                         return std::nullopt;
                     }
                     return json{{"frame", m_framesDrawn}};
                 };
                 return outcome;
             });

    Register(
        "wait_scene",
        "Answers once the scene, its textures, ray scene and streaming are ready, then {settle} more frames (default 2) with temporal effects restarted.",
        [this](const json& args)
        {
            const auto settle = ReadNumber<uint64_t>(args, "settle", 2);
            const uint64_t start = m_framesDrawn;
            auto ready = std::make_shared<std::optional<uint64_t>>();
            Outcome outcome;
            outcome.timeoutSeconds = ReadNumber<double>(args, "timeout_s", 900.0);
            outcome.wait = [this, settle, start, ready]() -> std::optional<json>
            {
                if (!ready->has_value())
                {
                    if (m_state.IsSceneLoading())
                    {
                        return std::nullopt;
                    }
                    *ready = m_framesDrawn;
                    ++m_state.temporalRestart;
                }
                if (m_framesDrawn < **ready + settle)
                {
                    return std::nullopt;
                }
                json result = {{"frame", m_framesDrawn}, {"frames_waited", m_framesDrawn - start}};
                if (!m_state.lastSceneIoError.empty())
                {
                    result["scene_error"] = m_state.lastSceneIoError;
                }
                return result;
            };
            return outcome;
        });

    Register(
        "deterministic",
        "Makes frames repeatable: every frame steps {frame_seconds} (default 1/60; 0 freezes clouds, time of day, animation, physics), "
        "exposure pinned at {exposure_ev100} (default: the exposure now), auto white balance off unless {auto_white_balance}, "
        "temporal history restarted. {enabled: false} goes back to real time and the camera's own adaptation.",
        [this, now](const json& args)
        {
            Camera& camera = m_state.camera;
            if (!args.value("enabled", true))
            {
                m_state.fixedFrameSeconds.reset();
                if (m_adaptationBeforeDeterministic.has_value())
                {
                    camera.autoExposure.enabled = m_adaptationBeforeDeterministic->first;
                    camera.autoWhiteBalance.enabled = m_adaptationBeforeDeterministic->second;
                    m_adaptationBeforeDeterministic.reset();
                }
                return now({{"enabled", false}});
            }
            const float frameSeconds = ReadNumber<float>(args, "frame_seconds", 1.0f / 60.0f);
            if (!(frameSeconds >= 0.0f) || frameSeconds > 1.0f)
            {
                throw std::runtime_error("'frame_seconds' takes 0 to 1");
            }
            if (!m_adaptationBeforeDeterministic.has_value())
            {
                m_adaptationBeforeDeterministic = std::make_pair(camera.autoExposure.enabled, camera.autoWhiteBalance.enabled);
            }
            m_state.fixedFrameSeconds = frameSeconds;
            camera.exposureEv100 = ReadNumber<float>(args, "exposure_ev100", camera.exposureEv100);
            camera.autoExposure.enabled = false;
            camera.autoWhiteBalance.enabled = args.value("auto_white_balance", false);
            ++m_state.temporalRestart;
            return now({
                {"enabled", true},
                {"frame_seconds", frameSeconds},
                {"exposure_ev100", camera.exposureEv100},
                {"auto_white_balance", camera.autoWhiteBalance.enabled}});
        });

    Register(
        "restart_temporal",
        "Starts TAA, AO, denoiser and atmosphere history over, as a scene load does; {full: true} also wipes the DDGI probes "
        "and redraws the sun's shadow cascades, so two captures after it match (A/B).",
        [this, now](const json& args)
        {
            if (args.value("full", false))
            {
                ++m_state.fullRestart;
            }
            else
            {
                ++m_state.temporalRestart;
            }
            return now(json::object());
        });

    Register("scene.load", "Opens the scene file {path} as File > Open would; wait_scene waits for it.", [this, now](const json& args)
             {
                 const std::string path = ReadString(args, "path");
                 const std::filesystem::path resolved = EnginePaths::ResolveProjectPath(path);
                 if (!std::filesystem::exists(resolved))
                 {
                     throw std::runtime_error("No scene file at '" + resolved.string() + "'");
                 }
                 m_state.lastSceneIoError.clear();
                 SceneIoService::StartAsyncSceneLoad(m_state, path);
                 return now({{"path", resolved.string()}});
             });

    Register("camera.get", "The camera: position, yaw, pitch, fov, exposure.", [this, now](const json&)
             {
                 return now(CameraToJson(m_state.camera));
             });

    Register(
        "camera.set",
        "Moves the camera: any of {position:[x,y,z], yaw, pitch, look_at:[x,y,z], fov, exposure_ev100 (turns auto exposure off), auto_exposure:bool}.",
        [this, now](const json& args)
        {
            Camera& camera = m_state.camera;
            if (args.contains("position"))
            {
                camera.position = ReadVec3(args["position"], "position");
            }
            camera.yawDegrees = ReadNumber<float>(args, "yaw", camera.yawDegrees);
            camera.pitchDegrees = ReadNumber<float>(args, "pitch", camera.pitchDegrees);
            if (args.contains("look_at"))
            {
                const glm::vec3 direction = ReadVec3(args["look_at"], "look_at") - camera.position;
                if (glm::length(direction) > 1e-6f)
                {
                    const glm::vec3 unit = glm::normalize(direction);
                    // Camera::GetForward: yaw about +Y from +X, pitch up from the horizon.
                    camera.yawDegrees = glm::degrees(std::atan2(unit.z, unit.x));
                    camera.pitchDegrees = glm::degrees(std::asin(std::clamp(unit.y, -1.0f, 1.0f)));
                }
            }
            camera.fovDegrees = ReadNumber<float>(args, "fov", camera.fovDegrees);
            if (args.contains("exposure_ev100"))
            {
                camera.exposureEv100 = ReadNumber<float>(args, "exposure_ev100", camera.exposureEv100);
                camera.autoExposure.enabled = false;
            }
            if (args.contains("auto_exposure"))
            {
                AssignField(camera.autoExposure.enabled, args["auto_exposure"], "auto_exposure");
            }
            return now(CameraToJson(camera));
        });

    Register(
        "render.get",
        "Render settings as flat keys (group.key), as the settings file names them; {prefix} narrows them.",
        [this, now](const json& args)
        {
            const std::string prefix = args.value("prefix", std::string());
            RenderDebugSettings render = m_state.editorUi.EditRenderDebug();
            Camera camera = m_state.camera;
            json result = json::object();
            VisitControlFields(render, camera, [&](const std::string& key, auto& field)
                               {
                                   if (key.rfind(prefix, 0) == 0)
                                   {
                                       result[key] = FieldToJson(field);
                                   }
                               });
            return now(result);
        });

    Register(
        "render.set",
        "Sets render settings: {values: {\"ray_tracing.reflections\": true, ...}} with render.get's keys; all or none apply.",
        [this, now](const json& args)
        {
            if (!args.contains("values") || !args["values"].is_object())
            {
                throw std::runtime_error("'values' (an object of key: value) is needed");
            }
            const json& values = args["values"];
            RenderDebugSettings render = m_state.editorUi.EditRenderDebug();
            Camera camera = m_state.camera;
            std::vector<std::string> applied;
            VisitControlFields(render, camera, [&](const std::string& key, auto& field)
                               {
                                   if (const auto found = values.find(key); found != values.end())
                                   {
                                       AssignField(field, *found, key);
                                       applied.push_back(key);
                                   }
                               });
            for (const auto& [key, value] : values.items())
            {
                if (std::find(applied.begin(), applied.end(), key) == applied.end())
                {
                    throw std::runtime_error("Unknown render setting '" + key + "' (render.get lists them)");
                }
            }
            m_state.editorUi.EditRenderDebug() = render;
            m_state.camera.autoExposure = camera.autoExposure;
            m_state.camera.autoWhiteBalance = camera.autoWhiteBalance;
            ++m_state.temporalRestart;
            return now({{"applied", applied}});
        });

    Register(
        "viewport.set",
        "Renders the scene at {width, height} whatever the editor layout; {width: 0} gives the size back to the viewport panel.",
        [this, now](const json& args)
        {
            const auto width = ReadNumber<uint32_t>(args, "width", 0);
            const auto height = ReadNumber<uint32_t>(args, "height", 0);
            if (width == 0 || height == 0)
            {
                m_state.fixedViewportExtent.reset();
            }
            else
            {
                m_state.fixedViewportExtent = RenderExtent{width, height};
            }
            return now({{"fixed", m_state.fixedViewportExtent.has_value()}});
        });

    Register("capture", "Writes the viewport's last frame to {path} (PNG).", [this, now](const json& args)
             {
                 const std::filesystem::path path = std::filesystem::absolute(ReadString(args, "path"));
                 std::filesystem::create_directories(path.parent_path());
                 m_renderer.CaptureViewport(path);
                 if (!std::filesystem::exists(path))
                 {
                     throw std::runtime_error("The capture was not written; the engine log says why");
                 }
                 return now({{"path", path.string()}, {"frame", m_framesDrawn}});
             });

    Register(
        "photo",
        "Photo Mode still to {path}: optional width, height, warmup, path_tracing, spp, samples, dlss, rr; answers once written.",
        [this](const json& args)
        {
            const PhotoModeSettings saved = ClampPhotoModeSettings(m_state.engineSettings.photoMode);
            IRenderBackend::PhotoRequest request = PhotoRequestFromSettings(saved);
            request.path = std::filesystem::absolute(ReadString(args, "path"));
            std::filesystem::create_directories(request.path.parent_path());
            request.width = ReadNumber<uint32_t>(args, "width", saved.width);
            request.height = ReadNumber<uint32_t>(args, "height", saved.height);
            request.warmupFrames = ReadNumber<uint32_t>(args, "warmup", saved.warmupFrames);
            if (args.contains("path_tracing"))
            {
                AssignField(request.offlinePathTracing, args["path_tracing"], "path_tracing");
            }
            request.samplesPerPixel = ReadNumber<uint32_t>(args, "spp", request.samplesPerPixel);
            request.targetSamples = ReadNumber<uint32_t>(args, "samples", request.targetSamples);
            if (args.contains("dlss"))
            {
                request.dlssMode = ParseDlssMode(ReadString(args, "dlss"));
            }
            if (args.contains("rr"))
            {
                AssignField(request.dlssRayReconstruction, args["rr"], "rr");
            }
            std::error_code ignored;
            std::filesystem::remove(request.path, ignored);
            std::string error;
            if (!m_renderer.TakePhoto(request, error))
            {
                throw std::runtime_error(error);
            }
            const uint64_t start = m_framesDrawn;
            const std::filesystem::path path = request.path;
            Outcome outcome;
            outcome.timeoutSeconds = ReadNumber<double>(args, "timeout_s", 1800.0);
            outcome.wait = [this, start, path]() -> std::optional<json>
            {
                if (m_framesDrawn == start || m_renderer.IsTakingPhoto())
                {
                    return std::nullopt;
                }
                m_renderer.WaitForPhotoWrite();
                if (!std::filesystem::exists(path))
                {
                    throw std::runtime_error("The photo was not written; the engine log says why");
                }
                return json{{"path", path.string()}, {"frames", m_framesDrawn - start}};
            };
            return outcome;
        });

    Register("timings", "Average CPU, main-thread and per-pass GPU times (ms) over the recent frames.", [this, now](const json&)
             {
                 return now(TimingsToJson(m_renderer.GetFrameTimings()));
             });

    Register("entities.list", "The scene's entities: id, name, kind, transform; {filter} keeps names that contain it.", [this, now](const json& args)
             {
                 const std::string filter = args.value("filter", std::string());
                 const IEditorWorld& world = m_state.GetEditorWorld();
                 json list = json::array();
                 for (const entt::entity entity : world.GetSceneOrder())
                 {
                     if (filter.empty() || world.GetTag(entity).name.find(filter) != std::string::npos)
                     {
                         list.push_back(EntityToJson(world, entity));
                     }
                 }
                 return now(list);
             });

    Register(
        "entity.set",
        "Moves an entity ({id} or {name}): any of position, rotation (degrees), scale as [x,y,z].",
        [this, now](const json& args)
        {
            IEditorWorld& world = m_state.GetEditorWorld();
            const entt::entity entity = FindEntity(world, args);
            TransformComponent& transform = world.EditTransform(entity);
            if (args.contains("position"))
            {
                transform.translation = ReadVec3(args["position"], "position");
            }
            if (args.contains("rotation"))
            {
                transform.rotationDegrees = ReadVec3(args["rotation"], "rotation");
            }
            if (args.contains("scale"))
            {
                transform.scale = ReadVec3(args["scale"], "scale");
            }
            world.MarkTransformDirty(entity);
            return now(EntityToJson(world, entity));
        });

    Register(
        "scene.get",
        "The scene as its file holds it, as JSON: all of it, or the part at {path} (dots; arrays by index or tag, "
        "e.g. 'lights.Key box', 'environment.clouds').",
        [this, now](const json& args)
        {
            json document = SceneDocument(m_state.GetEditorWorld());
            const std::string path = args.value("path", std::string());
            json* node = &document;
            for (const std::string& part : SplitPath(path))
            {
                node = &StepInto(*node, part, path);
            }
            return now(*node);
        });

    Register(
        "scene.set",
        "Changes the scene by scene.get's paths, {values: {'environment.clouds.coverage': 0.6, 'lights.Key box.intensity': 90000}}: "
        "environment, lights, the entities' tag and transform, drive_paths and minimap; all or none apply.",
        [this, now](const json& args)
        {
            if (!args.contains("values") || !args["values"].is_object())
            {
                throw std::runtime_error("'values' (an object of path: value) is needed");
            }
            IEditorWorld& world = m_state.GetEditorWorld();
            json document = SceneDocument(world);
            bool environment = false;
            bool drivePaths = false;
            bool minimap = false;
            std::vector<size_t> lights;
            std::vector<size_t> entities;
            for (const auto& [path, value] : args["values"].items())
            {
                const std::vector<std::string> parts = SplitPath(path);
                if (parts.empty())
                {
                    throw std::runtime_error("An empty path");
                }
                json* node = &document;
                size_t element = 0;
                for (size_t index = 0; index < parts.size(); ++index)
                {
                    json* parent = node;
                    node = &StepInto(*node, parts[index], path);
                    if (index == 1 && parent->is_array())
                    {
                        element = static_cast<size_t>(std::distance(parent->begin(), std::find_if(parent->begin(), parent->end(), [&](const json& candidate)
                                                                                                  {
                                                                                                      return &candidate == node;
                                                                                                  })));
                    }
                }
                if (!SameKind(*node, value))
                {
                    throw std::runtime_error("'" + path + "' is " + node->dump() + "; " + value.dump() + " is not of its kind");
                }
                const std::string& section = parts[0];
                if (section == "environment")
                {
                    environment = true;
                }
                else if (section == "drive_paths")
                {
                    drivePaths = true;
                }
                else if (section == "minimap")
                {
                    minimap = true;
                }
                else if (section == "lights" && parts.size() >= 3)
                {
                    lights.push_back(element);
                }
                else if (section == "entities" && parts.size() >= 3 && (parts[2] == "tag" || parts[2] == "transform"))
                {
                    entities.push_back(element);
                }
                else
                {
                    throw std::runtime_error("'" + path + "' cannot be changed while the scene is open (scene.load a file that has it)");
                }
                *node = value;
            }
            const SerializedSceneData data = ParseEditorSceneData(document.dump());
            const auto findByUuid = [&](const std::string& uuid)
            {
                for (const entt::entity entity : world.GetSceneOrder())
                {
                    if (world.GetEntityUuid(entity) == uuid)
                    {
                        return entity;
                    }
                }
                throw std::runtime_error("No entity has the uuid " + uuid);
            };
            if (environment)
            {
                world.SetEnvironment(data.environment);
            }
            if (drivePaths)
            {
                world.SetDrivePaths(data.drivePaths);
            }
            if (minimap)
            {
                world.SetMinimap(data.minimap);
            }
            for (const size_t index : lights)
            {
                const SerializedLightData& source = data.lights.at(index);
                const entt::entity entity = findByUuid(source.entityUuid);
                world.EditTag(entity).name = source.tagName;
                world.EditTransform(entity) = source.transform;
                world.MarkTransformDirty(entity);
                LightComponent& light = world.EditLightComponent(entity);
                light.type = source.lightType;
                light.color = source.color;
                light.intensity = source.intensity;
                light.range = source.range;
                light.spotInnerAngleDegrees = source.spotInnerAngle;
                light.spotOuterAngleDegrees = source.spotOuterAngle;
                light.areaSize = source.areaSize;
                light.castShadows = source.castShadows;
                light.sourceRadius = source.sourceRadius;
                light.groundColor = source.groundColor;
            }
            for (const size_t index : entities)
            {
                const SerializedEntityData& source = data.entities.at(index);
                const entt::entity entity = findByUuid(source.entityUuid);
                world.EditTag(entity).name = source.tagName;
                world.EditTransform(entity) = source.transform;
                world.MarkTransformDirty(entity);
            }
            return now({{"applied", args["values"].size()}});
        });

    Register("drive.start", "Drives the model entity {name} (or {id}) as Play would.", [this, now](const json& args)
             {
                 const entt::entity entity = FindEntity(m_state.GetEditorWorld(), args);
                 VehicleDriveService::Start(m_state, entity, VehicleDriveService::DefaultTuning());
                 return now(DriveStatusToJson(VehicleDriveService::GetStatus(m_state), false));
             });

    Register("drive.stop", "Stops driving; the car and camera go back to where they started.", [this, now](const json&)
             {
                 m_state.vehicleDrive.scriptedControls.reset();
                 VehicleDriveService::Stop(m_state);
                 return now(json::object());
             });

    Register("drive.reset", "Puts the car back at its start, stopped.", [this, now](const json&)
             {
                 VehicleDriveService::Reset(m_state);
                 return now(json::object());
             });

    Register(
        "drive.controls",
        "Holds {throttle (-1..1), steering (-1..1), brake, hand_brake} instead of the keyboard and gamepad; {release: true} gives them back.",
        [this, now](const json& args)
        {
            if (args.value("release", false))
            {
                m_state.vehicleDrive.scriptedControls.reset();
                return now({{"scripted", false}});
            }
            VehicleControls controls = m_state.vehicleDrive.scriptedControls.value_or(VehicleControls{});
            controls.throttle = std::clamp(ReadNumber<float>(args, "throttle", controls.throttle), -1.0f, 1.0f);
            controls.steering = std::clamp(ReadNumber<float>(args, "steering", controls.steering), -1.0f, 1.0f);
            controls.brake = std::clamp(ReadNumber<float>(args, "brake", controls.brake), 0.0f, 1.0f);
            controls.handBrake = std::clamp(ReadNumber<float>(args, "hand_brake", controls.handBrake), 0.0f, 1.0f);
            m_state.vehicleDrive.scriptedControls = controls;
            return now({{"scripted", true}});
        });

    Register("drive.pause", "Pauses ({paused: true}) or resumes the simulation.", [this, now](const json& args)
             {
                 VehicleDriveService::SetPaused(m_state, args.value("paused", true));
                 return now(json::object());
             });

    Register("drive.status", "The driven car: pose, speed, revs, gear, controls; {wheels: true} adds each wheel's forces and slips.", [this, now](const json& args)
             {
                 return now(DriveStatusToJson(VehicleDriveService::GetStatus(m_state), args.value("wheels", false)));
             });

    Register("log", "The engine log's last {lines} lines (default 100), only those after sequence {after}.", [now](const json& args)
             {
                 const auto lines = ReadNumber<size_t>(args, "lines", 100);
                 const auto after = ReadNumber<uint64_t>(args, "after", 0);
                 json list = json::array();
                 for (const Log::RecentLine& line : Log::RecentLines(lines, after))
                 {
                     list.push_back({{"seq", line.sequence}, {"text", line.text}});
                 }
                 return now(list);
             });

    Register(
        "debug.crash",
        "For checking crash reports: {kind: 'report'} writes one for the main thread and goes on; "
        "'access_violation', 'abort' or 'throw' crash the engine on purpose.",
        [now](const json& args)
        {
            const std::string kind = ReadString(args, "kind");
            if (kind == "report")
            {
                return now({{"report", platform::crash::WriteReportForCurrentThread("debug.crash report").string()}});
            }
            if (kind == "access_violation")
            {
                volatile int* nowhere = nullptr;
                *nowhere = 1;
            }
            else if (kind == "abort")
            {
                std::abort();
            }
            else if (kind == "throw")
            {
                ThrowOutOfNoexcept("debug.crash throw");
            }
            throw std::runtime_error("'kind' takes report, access_violation, abort or throw");
        });

    Register(
        "renderdoc.capture",
        "RenderDoc capture of the next {frames} frames (default 1); answers with the .rdc once written. Needs --renderdoc.",
        [this](const json& args)
        {
            if (!platform::renderdoc::IsLoaded())
            {
                throw std::runtime_error("RenderDoc is not loaded: start the engine with --renderdoc");
            }
            const size_t before = platform::renderdoc::Captures().size();
            platform::renderdoc::TriggerCapture(ReadNumber<uint32_t>(args, "frames", 1));
            Outcome outcome;
            outcome.timeoutSeconds = ReadNumber<double>(args, "timeout_s", 120.0);
            outcome.wait = [before]() -> std::optional<json>
            {
                const std::vector<platform::renderdoc::CaptureFile> captures = platform::renderdoc::Captures();
                if (captures.size() <= before || platform::renderdoc::IsCapturing())
                {
                    return std::nullopt;
                }
                const std::filesystem::path& path = captures.back().path;
                std::error_code error;
                return json{{"path", path.string()}, {"bytes", std::filesystem::file_size(path, error)}};
            };
            return outcome;
        });

    Register("renderdoc.list", "The RenderDoc captures of this run.", [now](const json&)
             {
                 json list = json::array();
                 for (const platform::renderdoc::CaptureFile& capture : platform::renderdoc::Captures())
                 {
                     list.push_back({{"path", capture.path.string()}, {"timestamp", capture.timestamp}});
                 }
                 return now({{"loaded", platform::renderdoc::IsLoaded()}, {"captures", list}});
             });

    Register("renderdoc.open", "Opens the RenderDoc UI on {path} (default: the latest capture), connected to the engine.", [now](const json& args)
             {
                 std::filesystem::path path = args.value("path", std::string());
                 if (path.empty())
                 {
                     const std::vector<platform::renderdoc::CaptureFile> captures = platform::renderdoc::Captures();
                     if (captures.empty())
                     {
                         throw std::runtime_error("No capture yet (renderdoc.capture)");
                     }
                     path = captures.back().path;
                 }
                 if (!platform::renderdoc::OpenInUi(path, true))
                 {
                     throw std::runtime_error("The RenderDoc UI did not start");
                 }
                 return now({{"path", path.string()}});
             });

    Register("crash_folder", "Where crash reports go.", [now](const json&)
             {
                 return now({{"folder", platform::crash::CrashFolder().string()}});
             });

    Register("ui.windows", "The editor's windows: name (the first part of a ui.run ref), shown, rect, docked.", [now](const json&)
             {
                 return now(ListUiWindows());
             });

    Register(
        "ui.run",
        "Drives the editor UI with simulated input (Dear ImGui Test Engine; the real mouse is not touched). {steps: [{op, ref, ...}]}; "
        "ops: click, right_click, double_click, hold{seconds}, check, uncheck, open, close, input{value}, menu (ref 'File/Open...'), combo, "
        "key{keys:'Ctrl+S'}, type{text}, focus, hover, drag{to}, drag_by{delta}, wheel{delta}, wait{seconds}, yield{frames}, set_ref, "
        "info, exists, read, list{depth}. Refs are ImGui paths: 'Window/Label', 'Window/##id', '**/Label'. Answers when done.",
        [this](const json& args)
        {
            // A test engine made now has not seen a frame yet and refuses tests until it has.
            uint64_t startFrame = m_framesDrawn;
            if (!m_uiRunner)
            {
                ImGuiTestEngine* engine = m_renderer.GetUiTestEngine();
                if (engine == nullptr)
                {
                    throw std::runtime_error("This build has no ImGui test engine");
                }
                m_uiRunner = std::make_unique<ControlUiRunner>(engine);
                startFrame += 2;
            }
            const json steps = args.value("steps", json());
            ControlUiRunner::Validate(steps);
            auto started = std::make_shared<bool>(false);
            Outcome outcome;
            outcome.timeoutSeconds = ReadNumber<double>(args, "timeout_s", 120.0);
            outcome.wait = [this, steps, startFrame, started]() -> std::optional<json>
            {
                if (!*started)
                {
                    if (m_framesDrawn < startFrame)
                    {
                        return std::nullopt;
                    }
                    m_uiRunner->Start(steps);
                    *started = true;
                    return std::nullopt;
                }
                if (!m_uiRunner->Done())
                {
                    return std::nullopt;
                }
                return m_uiRunner->Result();
            };
            return outcome;
        });

    Register("quit", "Closes the engine after this frame (an unsaved scene is not asked about).", [this, now](const json&)
             {
                 m_quitRequested = true;
                 return now(json::object());
             });
}
}
