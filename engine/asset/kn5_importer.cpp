#include "kn5_importer.h"

#include "ac_car_data.h"
#include "dds_decoder.h"
#include "kn5_reader.h"
#include "texture_loader.h"
#include "tyre_library.h"

#include <engine/audio/fmod_bank.h>
#include <engine/core/log/log.h>
#include <engine/core/text/ascii.h>
#include <engine/core/threading/task_system.h>

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace me
{

namespace
{
using Json = nlohmann::json;

constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;
// A texture blob smaller than this is a placeholder (or, encrypted, a decoy).
constexpr size_t kStubTextureBytes = 128;
// The least roughness a ksMultilayer surface (tarmac, grass, sand, kerbs) is imported with.
constexpr float kMultilayerMinRoughness = 0.7f;
// The clear coat's reflectance at normal incidence (KHR_materials_clearcoat: IOR 1.5).
constexpr float kCoatF0 = 0.04f;
// The least roughness written: the renderer's floor (deferred_lighting.frag clamps to 0.04).
constexpr float kMinRoughness = 0.04f;
// AC's clear-noon light (content/weather/3_clear/colorCurves.ini, HIGH) as a diffuse surface meets
// it: the sun's luminance 12.6 over a mean N.L of 0.5, the ambient's 4.5 over a mean hemisphere
// factor of 0.75. A neutral surface (ksDiffuse = ksAmbient = 0.5) gets kAcNeutralLight of it, and
// the same surface is about kAcNeutralLuminance cd/m^2 in daylight here (120 klx sun at a mean N.L
// of 0.5, ~20 klx of sky).
constexpr float kAcSunLight = 12.6f * 0.5f;
constexpr float kAcAmbientLight = 4.5f * 0.75f;
constexpr float kAcNeutralLight = 0.5f * (kAcSunLight + kAcAmbientLight);
constexpr float kAcNeutralLuminance = 25500.0f;
// The gain, in gamma space, a neutral surface is drawn at: the level Kunos road-car paint has been
// imported at (its detail colour x2), which reads closest to the game's own skin previews. AC's
// ratios between materials hold at any level; this one clips the brightest liveries (white, red,
// yellow) at 1, as the import always has.
constexpr float kAcNeutralGain = 2.0f;
// The glTF node extension that marks a mesh as collision only (see ModelCollisionMesh).
constexpr const char* kCollisionExtension = "MINIENGINE_collision";
// The glTF node extension that carries how the game draws a mesh: no shadow, and the camera distances
// it is drawn between (see ModelSubmeshData::castShadows and drawDistance).
constexpr const char* kMeshDrawExtension = "MINIENGINE_mesh_draw";
// The glTF extension, on the document, that carries a car's own figures (see VehicleCarSpec).
constexpr const char* kVehicleExtension = "MINIENGINE_vehicle";

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

bool EndsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsFinite(const float* values, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (!std::isfinite(values[index]))
        {
            return false;
        }
    }
    return true;
}

float Round(float value, int digits)
{
    const float scale = std::pow(10.0f, static_cast<float>(digits));
    return std::round(value * scale) / scale;
}

float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t LinearToSrgb8(float value)
{
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0f));
}

Json PointsToJson(const std::vector<glm::vec2>& points)
{
    Json out = Json::array();
    for (const glm::vec2& point : points)
    {
        out.push_back(Json::array({Round(point.x, 4), Round(point.y, 4)}));
    }
    return out;
}

Json ControllersToJson(const std::vector<VehicleController>& controllers)
{
    Json out = Json::array();
    for (const VehicleController& controller : controllers)
    {
        out.push_back(Json{{"input", controller.input},
                           {"combinator", controller.combinator},
                           {"curve", PointsToJson(controller.curve)},
                           {"filter", Round(controller.filter, 6)},
                           {"upLimit", Round(controller.upLimit, 4)},
                           {"downLimit", Round(controller.downLimit, 4)}});
    }
    return out;
}

Json NumbersToJson(const std::vector<float>& numbers)
{
    Json out = Json::array();
    for (const float number : numbers)
    {
        out.push_back(Round(number, 4));
    }
    return out;
}

Json NumberMapToJson(const std::map<std::string, float>& numbers)
{
    Json out = Json::object();
    for (const auto& [key, value] : numbers)
    {
        out[key] = Round(value, 6);
    }
    return out;
}

Json TyreSettingsToJson(const VehicleTyreSettings& tyres)
{
    return Json{{"longitudinalGrip", Round(tyres.longitudinalGrip, 4)},
                {"lateralGrip", Round(tyres.lateralGrip, 4)},
                {"peakSlipRatio", Round(tyres.peakSlipRatio, 4)},
                {"peakSlipAngleDegrees", Round(tyres.peakSlipAngleDegrees, 4)},
                {"postPeakShare", Round(tyres.postPeakShare, 4)},
                {"inertia", Round(tyres.inertia, 4)}};
}

Json TyreDataToJson(const VehicleTyreData& data)
{
    Json curves = Json::object();
    for (const auto& [key, points] : data.curves)
    {
        curves[key] = PointsToJson(points);
    }
    return Json{{"name", data.name}, {"shortName", data.shortName}, {"values", NumberMapToJson(data.values)}, {"curves", std::move(curves)}};
}

// One axle's suspension: its type, hardpoints (forward, outward, up from the wheel centre) and rates.
Json SuspensionAxleToJson(const VehicleSuspensionAxle& axle)
{
    Json out = Json::object();
    out["type"] = axle.type == VehicleSuspensionType::DoubleWishbone ? "doubleWishbone"
                  : axle.type == VehicleSuspensionType::MacPherson   ? "macPherson"
                  : axle.type == VehicleSuspensionType::SolidAxle    ? "solidAxle"
                                                                     : "none";
    // Small numbers (hardpoints, a toe rod's tenths of a millimetre) to the micrometre; rates and
    // masses to two places, which a float scaled by a million would not hold.
    const auto number = [](float value)
    {
        return std::abs(value) >= 100.0f ? Round(value, 2) : Round(value, 6);
    };
    const auto point = [&out](const char* key, const glm::vec3& value)
    {
        out[key] = Json::array({Round(value.x, 6), Round(value.y, 6), Round(value.z, 6)});
    };
    point("lowerFront", axle.lowerFront);
    point("lowerRear", axle.lowerRear);
    point("lowerBall", axle.lowerBall);
    point("upperFront", axle.upperFront);
    point("upperRear", axle.upperRear);
    point("upperBall", axle.upperBall);
    point("strutTop", axle.strutTop);
    point("strutLower", axle.strutLower);
    point("tieInner", axle.tieInner);
    point("tieOuter", axle.tieOuter);
    out["staticCamberDegrees"] = number(axle.staticCamberDegrees);
    out["toeOutRodLength"] = number(axle.toeOutRodLength);
    out["track"] = number(axle.track);
    out["wheelRate"] = number(axle.wheelRate);
    out["progressiveRate"] = number(axle.progressiveRate);
    out["bumpStopRate"] = number(axle.bumpStopRate);
    out["bumpStopTravel"] = number(axle.bumpStopTravel);
    out["reboundStopTravel"] = number(axle.reboundStopTravel);
    if (axle.rodLength.has_value())
    {
        out["rodLength"] = number(*axle.rodLength);
    }
    if (axle.packerRange.has_value())
    {
        out["packerRange"] = number(*axle.packerRange);
    }
    out["dampBump"] = number(axle.dampBump);
    out["dampFastBump"] = number(axle.dampFastBump);
    out["dampFastBumpThreshold"] = number(axle.dampFastBumpThreshold);
    out["dampRebound"] = number(axle.dampRebound);
    out["dampFastRebound"] = number(axle.dampFastRebound);
    out["dampFastReboundThreshold"] = number(axle.dampFastReboundThreshold);
    out["antiRollBarRate"] = number(axle.antiRollBarRate);
    out["hubMass"] = number(axle.hubMass);
    out["tyreRadius"] = number(axle.tyreRadius);
    out["tyreRate"] = number(axle.tyreRate);
    out["tyreDamping"] = number(axle.tyreDamping);
    out["centerOfMassAboveWheel"] = number(axle.centerOfMassAboveWheel);
    out["frictionCoulomb"] = number(axle.frictionCoulomb);
    out["frictionBreakaway"] = number(axle.frictionBreakaway);
    if (axle.type == VehicleSuspensionType::SolidAxle)
    {
        Json links = Json::array();
        for (const VehicleAxleLink& link : axle.axleLinks)
        {
            links.push_back(Json::array({Round(link.chassis.x, 6), Round(link.chassis.y, 6), Round(link.chassis.z, 6), Round(link.axle.x, 6),
                                         Round(link.axle.y, 6), Round(link.axle.z, 6)}));
        }
        out["axleLinks"] = std::move(links);
        out["axleSpringPosition"] = number(axle.axleSpringPosition);
        out["axleLateralStiffness"] = number(axle.axleLateralStiffness);
        out["axleTorqueReaction"] = number(axle.axleTorqueReaction);
    }
    return out;
}

// A car's figures as the MINIENGINE_vehicle extension: SI units, a member for each figure the car's
// data gave.
Json CarSpecToJson(const VehicleCarSpec& spec)
{
    Json out = Json::object();
    const auto put = [&out](const char* key, const std::optional<float>& value)
    {
        if (value.has_value())
        {
            out[key] = Round(*value, 4);
        }
    };
    put("massKg", spec.massKg);
    if (spec.drive.has_value())
    {
        out["drive"] = *spec.drive == VehicleDrive::RearWheel ? "rear" : *spec.drive == VehicleDrive::FrontWheel ? "front"
                                                                                                                 : "all";
    }
    if (!spec.torqueCurve.empty())
    {
        out["torqueCurve"] = PointsToJson(spec.torqueCurve);
    }
    put("minRpm", spec.minRpm);
    put("maxRpm", spec.maxRpm);
    if (!spec.gearRatios.empty())
    {
        out["gearRatios"] = NumbersToJson(spec.gearRatios);
    }
    put("reverseGearRatio", spec.reverseGearRatio);
    put("finalDriveRatio", spec.finalDriveRatio);
    put("gearSwitchSeconds", spec.gearSwitchSeconds);
    put("clutchReleaseSeconds", spec.clutchReleaseSeconds);
    put("engineInertia", spec.engineInertia);
    if (spec.frontTyres.has_value())
    {
        out["frontTyres"] = TyreSettingsToJson(*spec.frontTyres);
    }
    if (spec.rearTyres.has_value())
    {
        out["rearTyres"] = TyreSettingsToJson(*spec.rearTyres);
    }
    put("maxSteerAngleDegrees", spec.maxSteerAngleDegrees);
    put("steeringWheelLockDegrees", spec.steeringWheelLockDegrees);
    put("brakeTorquePerWheel", spec.brakeTorquePerWheel);
    put("frontBrakeShare", spec.frontBrakeShare);
    put("handBrakeTorquePerWheel", spec.handBrakeTorquePerWheel);
    put("suspensionFrequencyHz", spec.suspensionFrequencyHz);
    put("suspensionDamping", spec.suspensionDamping);
    if (spec.antiRollBars.has_value())
    {
        out["antiRollBars"] = *spec.antiRollBars;
    }
    if (spec.limitedSlipDifferentials.has_value())
    {
        out["limitedSlipDifferentials"] = *spec.limitedSlipDifferentials;
    }
    if (spec.frontSuspension.has_value())
    {
        out["frontSuspension"] = SuspensionAxleToJson(*spec.frontSuspension);
    }
    if (spec.rearSuspension.has_value())
    {
        out["rearSuspension"] = SuspensionAxleToJson(*spec.rearSuspension);
    }
    put("wheelbase", spec.wheelbase);
    put("frontWeightShare", spec.frontWeightShare);
    if (spec.inertiaBox.has_value())
    {
        out["inertiaBox"] = Json::array({Round(spec.inertiaBox->x, 4), Round(spec.inertiaBox->y, 4), Round(spec.inertiaBox->z, 4)});
    }
    put("fuelLitres", spec.fuelLitres);
    if (spec.fuelTankPosition.has_value())
    {
        out["fuelTankPosition"] = Json::array({Round(spec.fuelTankPosition->x, 4), Round(spec.fuelTankPosition->y, 4), Round(spec.fuelTankPosition->z, 4)});
    }
    if (spec.cockpitCamera.has_value())
    {
        const VehicleCameraMount& eyes = *spec.cockpitCamera;
        out["cockpitCamera"] = Json{
            {"position", Json::array({Round(eyes.position.x, 4), Round(eyes.position.y, 4), Round(eyes.position.z, 4)})},
            {"pitchDegrees", Round(eyes.pitchDegrees, 3)}};
    }

    if (!spec.aeroWings.empty())
    {
        Json wings = Json::array();
        for (const VehicleAeroWing& wing : spec.aeroWings)
        {
            Json curves = Json::object();
            for (const auto& [key, points] : wing.curves)
            {
                curves[key] = PointsToJson(points);
            }
            wings.push_back(Json{{"name", wing.name},
                                 {"chord", Round(wing.chord, 4)},
                                 {"span", Round(wing.span, 4)},
                                 {"position", Json::array({Round(wing.position.x, 4), Round(wing.position.y, 4), Round(wing.position.z, 4)})},
                                 {"angleDegrees", Round(wing.angleDegrees, 4)},
                                 {"liftGain", Round(wing.liftGain, 4)},
                                 {"dragGain", Round(wing.dragGain, 4)},
                                 {"liftCurve", PointsToJson(wing.liftCurve)},
                                 {"dragCurve", PointsToJson(wing.dragCurve)},
                                 {"curves", std::move(curves)},
                                 {"values", NumberMapToJson(wing.values)}});
        }
        out["aeroWings"] = std::move(wings);
    }
    if (!spec.aeroControllers.empty())
    {
        Json controllers = Json::array();
        for (const VehicleAeroController& controller : spec.aeroControllers)
        {
            controllers.push_back(Json{{"wing", controller.wing},
                                       {"input", controller.input},
                                       {"combinator", controller.combinator},
                                       {"curve", PointsToJson(controller.curve)},
                                       {"filter", Round(controller.filter, 6)},
                                       {"upLimit", Round(controller.upLimit, 4)},
                                       {"downLimit", Round(controller.downLimit, 4)}});
        }
        out["aeroControllers"] = std::move(controllers);
    }

    if (!spec.tyreCompounds.empty())
    {
        Json compounds = Json::array();
        for (const VehicleTyreCompound& compound : spec.tyreCompounds)
        {
            compounds.push_back(Json{{"front", TyreDataToJson(compound.front)}, {"rear", TyreDataToJson(compound.rear)}});
        }
        out["tyreCompounds"] = std::move(compounds);
    }
    if (spec.defaultTyreCompound.has_value())
    {
        out["defaultTyreCompound"] = *spec.defaultTyreCompound;
    }
    if (Json tyres = TyreLibrary::CarTyresToJson(spec); !tyres.is_null())
    {
        out["tyres"] = std::move(tyres);
    }
    if (!spec.turbos.empty())
    {
        Json turbos = Json::array();
        for (const VehicleTurbo& turbo : spec.turbos)
        {
            turbos.push_back(Json{{"maxBoost", Round(turbo.maxBoost, 4)},
                                  {"wastegate", Round(turbo.wastegate, 4)},
                                  {"referenceRpm", Round(turbo.referenceRpm, 2)},
                                  {"gamma", Round(turbo.gamma, 4)},
                                  {"lagUp", Round(turbo.lagUp, 6)},
                                  {"lagDown", Round(turbo.lagDown, 6)}});
        }
        out["turbos"] = std::move(turbos);
    }
    if (spec.ers.has_value())
    {
        const VehicleErs& ers = *spec.ers;
        Json profiles = Json::array();
        for (const VehicleErsProfile& profile : ers.profiles)
        {
            profiles.push_back(Json{{"name", profile.name}, {"controllers", ControllersToJson(profile.controllers)}});
        }
        out["ers"] = Json{{"torqueCurve", PointsToJson(ers.torqueCurve)},
                          {"coastCurve", PointsToJson(ers.coastCurve)},
                          {"chargeK", Round(ers.chargeK, 8)},
                          {"dischargeSeconds", Round(ers.dischargeSeconds, 4)},
                          {"maxKjPerLap", Round(ers.maxKjPerLap, 2)},
                          {"hasButtonOverride", ers.hasButtonOverride},
                          {"brakeRearCorrection", Round(ers.brakeRearCorrection, 4)},
                          {"heatChargeK", Round(ers.heatChargeK, 8)},
                          {"heatTorquePercent", Round(ers.heatTorquePercent, 4)},
                          {"defaultProfile", ers.defaultProfile},
                          {"profiles", std::move(profiles)}};
    }
    if (spec.allWheelDrive.has_value())
    {
        const VehicleAllWheelDrive& awd = *spec.allWheelDrive;
        out["allWheelDrive"] = Json{{"coupling", awd.coupling},
                                    {"frontShare", Round(awd.frontShare, 4)},
                                    {"frontDiffPower", Round(awd.frontDiffPower, 4)},
                                    {"frontDiffCoast", Round(awd.frontDiffCoast, 4)},
                                    {"frontDiffPreload", Round(awd.frontDiffPreload, 2)},
                                    {"centreDiffPower", Round(awd.centreDiffPower, 4)},
                                    {"centreDiffCoast", Round(awd.centreDiffCoast, 4)},
                                    {"centreDiffPreload", Round(awd.centreDiffPreload, 2)},
                                    {"rearDiffPower", Round(awd.rearDiffPower, 4)},
                                    {"rearDiffCoast", Round(awd.rearDiffCoast, 4)},
                                    {"rearDiffPreload", Round(awd.rearDiffPreload, 2)},
                                    {"centreRampTorque", Round(awd.centreRampTorque, 2)},
                                    {"centreMaxTorque", Round(awd.centreMaxTorque, 2)},
                                    {"centreControllers", ControllersToJson(awd.centreControllers)}};
    }
    if (!spec.rearSteerControllers.empty())
    {
        out["rearSteerControllers"] = ControllersToJson(spec.rearSteerControllers);
    }
    if (!spec.colliders.empty())
    {
        Json colliders = Json::array();
        for (const VehicleColliderBox& box : spec.colliders)
        {
            colliders.push_back(Json{{"center", Json::array({Round(box.center.x, 4), Round(box.center.y, 4), Round(box.center.z, 4)})},
                                     {"size", Json::array({Round(box.size.x, 4), Round(box.size.y, 4), Round(box.size.z, 4)})},
                                     {"groundEnabled", box.groundEnabled}});
        }
        out["colliders"] = std::move(colliders);
    }
    if (!spec.colliderHull.empty())
    {
        Json hull = Json::array();
        for (const glm::vec3& point : spec.colliderHull)
        {
            hull.push_back(Json::array({Round(point.x, 4), Round(point.y, 4), Round(point.z, 4)}));
        }
        out["colliderHull"] = std::move(hull);
    }
    put("coastRpm", spec.coastRpm);
    put("coastTorque", spec.coastTorque);
    put("changeUpSeconds", spec.changeUpSeconds);
    put("changeDownSeconds", spec.changeDownSeconds);
    put("autoCutoffSeconds", spec.autoCutoffSeconds);
    put("clutchMaxTorque", spec.clutchMaxTorque);
    put("autoClutchMinRpm", spec.autoClutchMinRpm);
    put("autoClutchMaxRpm", spec.autoClutchMaxRpm);
    put("autoShiftUpRpm", spec.autoShiftUpRpm);
    put("autoShiftDownRpm", spec.autoShiftDownRpm);
    if (!spec.upshiftClutchProfile.empty())
    {
        out["upshiftClutchProfile"] = NumbersToJson(spec.upshiftClutchProfile);
    }
    if (!spec.downshiftClutchProfile.empty())
    {
        out["downshiftClutchProfile"] = NumbersToJson(spec.downshiftClutchProfile);
    }
    put("differentialPower", spec.differentialPower);
    put("differentialCoast", spec.differentialCoast);
    put("differentialPreload", spec.differentialPreload);
    if (!spec.electronics.empty())
    {
        Json electronics = Json::object();
        for (const auto& [section, numbers] : spec.electronics)
        {
            electronics[section] = NumberMapToJson(numbers);
        }
        out["electronics"] = std::move(electronics);
    }
    return out;
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error("Cannot create '" + path.string() + "'");
    }
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file)
    {
        throw std::runtime_error("Cannot write '" + path.string() + "'");
    }
}

// A texture blob as RGBA8, rows top-down: DDS through the engine's decoder, anything else
// (PNG, JPEG, TGA) through stb_image. nullopt, with a warning, when it cannot be decoded.
std::optional<TextureData> DecodeTextureBlob(const std::vector<std::uint8_t>& blob, const std::string& name)
{
    try
    {
        if (DdsDecoder::IsDds(blob.data(), blob.size()))
        {
            return DdsDecoder::Decode(blob.data(), blob.size(), name);
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            blob.data(), static_cast<int>(blob.size()), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr)
        {
            throw std::runtime_error(std::string("unrecognised image data (") + stbi_failure_reason() + ")");
        }
        TextureData image;
        image.width = width;
        image.height = height;
        image.channelCount = 4;
        image.pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        stbi_image_free(pixels);
        return image;
    }
    catch (const std::exception& error)
    {
        LOG_WARN("kn5 texture '{}' cannot be decoded: {}", name, error.what());
        return std::nullopt;
    }
}

void AppendToVector(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// Written through a memory buffer and std::ofstream so a non-ASCII path works on Windows too.
void WritePng(const std::filesystem::path& path, int width, int height, int channels, const std::uint8_t* pixels)
{
    std::vector<std::uint8_t> encoded;
    if (stbi_write_png_to_func(AppendToVector, &encoded, width, height, channels, pixels, width * channels) == 0)
    {
        throw std::runtime_error("Cannot encode '" + path.string() + "' as PNG");
    }
    WriteFileBytes(path, encoded.data(), encoded.size());
}

// A file name the texture can be written under: the kn5 name's stem with anything a filesystem
// or a URI might trip on replaced.
std::string SafeStem(const std::string& textureName)
{
    std::string stem = std::filesystem::path(textureName).stem().string();
    for (char& character : stem)
    {
        const unsigned char code = static_cast<unsigned char>(character);
        if (!(std::isalnum(code) || character == '_' || character == '-' || character == '.'))
        {
            character = '_';
        }
    }
    return stem.empty() ? std::string("texture") : stem;
}

// Assetto Corsa matches texture names ignoring case, so a kn5 can list "INT_DEcals.dds" and a
// material ask for "INT_Decals.dds". Entries that differ only in case collapse into the one with
// the largest blob (never the 1x1 placeholder), and every material slot is pointed at it.
// Returns how many entries were folded away.
size_t FoldTextureCase(Kn5Model& model)
{
    std::unordered_map<std::string, size_t> keep;
    for (size_t index = 0; index < model.textures.size(); ++index)
    {
        const std::string key = ToLowerAscii(model.textures[index].name);
        const auto found = keep.find(key);
        if (found == keep.end() || model.textures[index].data.size() > model.textures[found->second].data.size())
        {
            keep[key] = index;
        }
    }
    // Material slots are repointed even when nothing folded: one table entry can still differ in
    // case from the name a material asks for.
    const size_t folded = model.textures.size() - keep.size();
    std::unordered_map<std::string, std::string> canonical;
    std::vector<size_t> kept;
    for (const auto& [key, index] : keep)
    {
        canonical[key] = model.textures[index].name;
        kept.push_back(index);
    }
    std::sort(kept.begin(), kept.end());
    std::vector<Kn5Texture> textures;
    textures.reserve(kept.size());
    for (size_t index : kept)
    {
        textures.push_back(std::move(model.textures[index]));
    }
    model.textures = std::move(textures);

    for (Kn5Material& material : model.materials)
    {
        for (auto& [slot, name] : material.textures)
        {
            const auto found = canonical.find(ToLowerAscii(name));
            if (found != canonical.end())
            {
                name = found->second;
            }
        }
    }
    return folded;
}

std::optional<std::filesystem::path> ResolveSkinDirectory(const std::filesystem::path& kn5Path, const std::string& wanted)
{
    if (ToLowerAscii(wanted) == "none")
    {
        return std::nullopt;
    }
    const std::vector<std::string> skins = Kn5Importer::ListSkins(kn5Path);
    const std::filesystem::path root = kn5Path.parent_path() / "skins";
    if (wanted.empty())
    {
        return skins.empty() ? std::nullopt : std::optional<std::filesystem::path>(root / skins.front());
    }
    for (const std::string& skin : skins)
    {
        if (ToLowerAscii(skin) == ToLowerAscii(wanted))
        {
            return root / skin;
        }
    }
    std::string have;
    for (const std::string& skin : skins)
    {
        have += (have.empty() ? "" : ", ") + skin;
    }
    throw std::runtime_error(
        "No skin '" + wanted + "' in '" + root.string() + "'" + (have.empty() ? std::string(" (it has none)") : " - have: " + have));
}

// Overwrites kn5 texture blobs with the same-named files from the skin folder (ignoring case), as
// the game does at load time. Not only the albedo: a livery also ships *_MAP.dds and plate or
// badge sheets. Returns the names replaced.
std::unordered_set<std::string> ApplySkin(Kn5Model& model, const std::filesystem::path& skinDirectory)
{
    std::unordered_map<std::string, std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(skinDirectory, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc))
        {
            files[ToLowerAscii(it->path().filename().string())] = it->path();
        }
    }

    std::unordered_set<std::string> swapped;
    for (Kn5Texture& texture : model.textures)
    {
        const auto found = files.find(ToLowerAscii(texture.name));
        if (found == files.end())
        {
            continue;
        }
        try
        {
            texture.data = ReadFileBytes(found->second);
            swapped.insert(texture.name);
        }
        catch (const std::exception& error)
        {
            LOG_WARN("Skin texture '{}' was not applied: {}", found->second.string(), error.what());
        }
    }
    return swapped;
}

const Kn5Texture* FindTexture(const Kn5Model& model, const std::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }
    for (const Kn5Texture& texture : model.textures)
    {
        if (texture.name == name)
        {
            return &texture;
        }
    }
    return nullptr;
}

std::array<float, 3> Normalized(const std::array<float, 3>& value, const std::array<float, 3>& fallback)
{
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!std::isfinite(length) || length < 1e-8f)
    {
        return fallback;
    }
    return {value[0] / length, value[1] / length, value[2] / length};
}

// Any unit vector perpendicular to `normal`: a stand-in for a tangent the source lost.
std::array<float, 3> Perpendicular(const std::array<float, 3>& normal)
{
    const std::array<float, 3> axis =
        std::abs(normal[0]) < 0.9f ? std::array<float, 3>{1.0f, 0.0f, 0.0f} : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const std::array<float, 3> cross{
        axis[1] * normal[2] - axis[2] * normal[1],
        axis[2] * normal[0] - axis[0] * normal[2],
        axis[0] * normal[1] - axis[1] * normal[0]};
    return Normalized(cross, {1.0f, 0.0f, 0.0f});
}

class GltfBuilder
{
  public:
    GltfBuilder(const std::filesystem::path& textureDirectory, const Kn5ImportOptions& options, std::vector<Kn5Surface> surfaces)
        : m_textureDirectory(textureDirectory), m_options(options), m_surfaces(std::move(surfaces))
    {
    }

    Kn5ImportReport& Report()
    {
        return m_report;
    }

    // Records which textures a model's materials sample. Called for every model of the import
    // before any texture is written: a track's add-on kn5 samples textures its main kn5 carries.
    // Names are matched ignoring case, as the game does across all the models it loads.
    void CollectTextureUse(const Kn5Model& model)
    {
        // A diffuse keeps its alpha only where the material reads it (blended or alpha-tested):
        // elsewhere AC never samples it, and several cars carry a fully transparent one on opaque
        // paint and interior maps.
        for (const Kn5Material& material : model.materials)
        {
            for (const char* slot : {"txDiffuse", "txNormal"})
            {
                const std::string name = material.Texture(slot);
                if (name.empty() || (std::string(slot) == "txNormal" && IsDentMap(name)))
                {
                    continue;
                }
                m_usedTextures.insert(ToLowerAscii(name));
                m_plainTextures.insert(ToLowerAscii(name));
                if (material.alphaBlend || material.alphaTested)
                {
                    m_alphaTextures.insert(ToLowerAscii(name));
                }
            }
            // A ksPerPixelMultiMap detail: written only if it turns out to be a pattern (see
            // WriteTextures), as a flat one folds into the base colour. Tables carry no texels.
            if (!IsMultilayer(material) && material.Property("useDetail", 0.0f) > 0.0f)
            {
                for (const char* slot : {"txDetail", "txNormalDetail"})
                {
                    if (const std::string name = material.Texture(slot); !name.empty())
                    {
                        m_usedTextures.insert(ToLowerAscii(name));
                    }
                }
            }
            if (IsMultilayer(material))
            {
                // The mask's alpha weighs the fourth layer.
                m_alphaTextures.insert(ToLowerAscii(material.Texture("txMask")));
                for (const char* slot : kMultilayerSlots)
                {
                    if (const std::string name = material.Texture(slot); !name.empty())
                    {
                        m_usedTextures.insert(ToLowerAscii(name));
                        m_multilayerTextures.insert(ToLowerAscii(name));
                    }
                }
            }
        }
    }

    // Writes the sampled textures this model carries and no earlier model already wrote: the game
    // keeps one texture per name. Decoding a DDS and encoding its PNG are independent per texture
    // and the bulk of an import, so they run on several threads; which textures are written and
    // under which names is settled first, on the calling thread, so the result does not depend on
    // the schedule. `progress` hears the fraction of the model's textures written, one at a time.
    void WriteTextures(const Kn5Model& model, const ImportProgressCallback& progress = {})
    {
        struct Job
        {
            const Kn5Texture* texture = nullptr;
            std::string key;
            std::string fileName;
            bool written = false;
        };

        std::vector<Job> jobs;
        for (const Kn5Texture& texture : model.textures)
        {
            const std::string key = ToLowerAscii(texture.name);
            if (!m_usedTextures.count(key) || texture.data.size() < kStubTextureBytes || m_textureUris.count(key) != 0)
            {
                continue;
            }
            if (!m_plainTextures.count(key) && !m_multilayerTextures.count(key) && DetailColor(model, texture.name).has_value())
            {
                continue; // a flat txDetail: a base-colour factor, not a map
            }
            // Reserved now so a later texture of this model sees the name taken; a texture that
            // then fails to decode gives it back below.
            std::string fileName = UniqueFileName(SafeStem(texture.name), ".png");
            m_textureUris[key] = "textures/" + fileName;
            jobs.push_back(Job{&texture, key, std::move(fileName), false});
        }
        if (jobs.empty())
        {
            return;
        }

        // A texture is a few tens of megabytes decoded; the cap keeps a many-core machine from
        // holding that many at once.
        constexpr size_t kMaxWriterThreads = 8;
        const size_t threadCount =
            std::min({jobs.size(), kMaxWriterThreads, static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()))});

        std::atomic<size_t> nextJob{0};
        std::atomic<bool> failed{false};
        std::mutex progressMutex;
        size_t finished = 0;
        const auto work = [&]
        {
            for (size_t index = nextJob.fetch_add(1); index < jobs.size() && !failed.load(); index = nextJob.fetch_add(1))
            {
                try
                {
                    Job& job = jobs[index];
                    job.written = WriteTexture(*job.texture, job.key, job.fileName);
                }
                catch (...)
                {
                    failed.store(true);
                    throw;
                }
                if (progress)
                {
                    // Under the lock, so the callback is never entered twice at once and the
                    // fractions it hears only grow.
                    const std::lock_guard<std::mutex> lock(progressMutex);
                    progress(static_cast<float>(++finished) / static_cast<float>(jobs.size()));
                }
            }
        };

        // The writers, as tasks; the calling thread is one of them. An import is background work.
        TaskSystem::ParallelFor(
            static_cast<uint32_t>(threadCount),
            1,
            [&](uint32_t begin, uint32_t end)
            {
                for (uint32_t writer = begin; writer < end; ++writer)
                {
                    work();
                }
            },
            TaskPriority::Low);

        for (const Job& job : jobs)
        {
            if (!job.written)
            {
                m_textureUris.erase(job.key);
                m_fileNames.erase(ToLowerAscii(job.fileName));
            }
        }
    }

    // Appends the model's materials; its meshes, emitted next, index them from here on.
    void AddMaterials(const Kn5Model& model)
    {
        m_materialBase = m_materials.size();
        m_materialCount = model.materials.size();
        for (const Kn5Material& material : model.materials)
        {
            m_materials.push_back(ConvertMaterial(model, material));
        }
        m_report.materials = m_materials.size();
    }

    // One kn5 node as one glTF node, children and all; nullopt when it is a dropped variant or a
    // childless mesh the game never draws.
    std::optional<size_t> Emit(const Kn5Node& node)
    {
        // The game collides cars with its physics meshes and draws none of them: they are kept, as
        // collision only.
        const bool collision = node.HasGeometry() && !node.renderable && Kn5Importer::IsPhysicsMeshName(node.name);
        const bool hidden = node.HasGeometry() && !collision && (!node.renderable || Kn5Importer::IsTrackMarker(node.name));
        if (hidden)
        {
            ++m_report.hiddenMeshes;
            if (node.children.empty())
            {
                return std::nullopt;
            }
        }
        if (!m_options.keepVariants && (Kn5Importer::IsRuntimeVariant(node.name) || m_lowRes.count(node.name) != 0))
        {
            ++m_report.droppedVariants;
            return std::nullopt;
        }

        Json gltfNode = Json::object();
        gltfNode["name"] = node.name;
        if (node.type == Kn5NodeType::Dummy)
        {
            ++m_report.transforms;
            if (node.matrix != kIdentity)
            {
                if (IsFinite(node.matrix.data(), node.matrix.size()))
                {
                    gltfNode["matrix"] = node.matrix;
                }
                else
                {
                    ++m_report.scrubbedMatrices;
                }
            }
        }
        else if (collision)
        {
            if (const std::optional<size_t> mesh = EmitCollisionMesh(node); mesh.has_value())
            {
                const Kn5Surface surface = Kn5Importer::MatchSurface(m_surfaces, node.name);
                gltfNode["mesh"] = *mesh;
                gltfNode["extensions"][kCollisionExtension] = {{"surface", surface.key}, {"friction", Round(surface.friction, 4)}};
                m_extensionsUsed.insert(kCollisionExtension);
            }
        }
        else if (hidden)
        {
            // Kept only as the parent of its children.
        }
        else if (const std::optional<size_t> mesh = EmitMesh(node); mesh.has_value())
        {
            gltfNode["mesh"] = *mesh;
            AddMeshDraw(node, gltfNode);
        }

        const size_t index = m_nodes.size();
        m_nodes.push_back(std::move(gltfNode));
        Json children = Json::array();
        for (const Kn5Node& child : node.children)
        {
            if (const std::optional<size_t> childIndex = Emit(child); childIndex.has_value())
            {
                children.push_back(*childIndex);
            }
        }
        if (!children.empty())
        {
            m_nodes[index]["children"] = std::move(children);
        }
        return index;
    }

    // What the kn5 says about drawing a mesh beyond its geometry, as MINIENGINE_mesh_draw: castShadows
    // false, and the camera distances it is drawn between (lodIn and lodOut, each left out where it
    // sets no limit). The node's layer, the World Detail level from which the game draws it, is not
    // carried: the engine draws every level, as the game at full detail does.
    void AddMeshDraw(const Kn5Node& node, Json& gltfNode)
    {
        Json draw = Json::object();
        if (!node.castShadows)
        {
            draw["castShadows"] = false;
            ++m_report.shadowlessMeshes;
        }
        const bool hasMin = std::isfinite(node.lodIn) && node.lodIn > 0.0f;
        const bool hasMax = std::isfinite(node.lodOut) && node.lodOut > 0.0f;
        if (hasMin)
        {
            draw["minDistance"] = Round(node.lodIn, 3);
        }
        if (hasMax)
        {
            draw["maxDistance"] = Round(node.lodOut, 3);
        }
        if (hasMin || hasMax)
        {
            ++m_report.distanceLimitedMeshes;
        }
        if (!draw.empty())
        {
            gltfNode["extensions"][kMeshDrawExtension] = std::move(draw);
            m_extensionsUsed.insert(kMeshDrawExtension);
        }
    }

    void SetLowResTwins(std::set<std::string> names)
    {
        m_lowRes = std::move(names);
    }

    // A layout model's placement, as a parent of its root node.
    size_t AddPlacement(const std::string& name, const std::array<float, 16>& matrix, size_t child)
    {
        m_nodes.push_back(Json{{"name", name}, {"matrix", matrix}, {"children", Json::array({child})}});
        return m_nodes.size() - 1;
    }

    size_t AddRoot(const std::string& name, const std::vector<size_t>& children)
    {
        // The half turn about Y that takes AC's axes (+X left, +Z forward) to glTF's (+X right,
        // -Z forward). Its determinant is +1, so winding and normals stay as they are.
        Json root = Json::object();
        root["name"] = name;
        root["matrix"] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
        if (!children.empty())
        {
            root["children"] = children;
        }
        m_nodes.push_back(std::move(root));
        return m_nodes.size() - 1;
    }

    // A car's materials are converted with AC's light scale (docs/design/
    // 2026-10-06-ac-light-scale-design.md); a track's are not calibrated against it yet. Call before
    // AddMaterials.
    void SetCarLighting(bool car)
    {
        m_carLighting = car;
    }

    // A car's own figures, written on the document as MINIENGINE_vehicle.
    void SetVehicle(const VehicleCarSpec& spec)
    {
        m_vehicle = CarSpecToJson(spec);
    }

    Json BuildDocument(size_t rootNode, const std::string& binaryUri) const
    {
        Json document = Json::object();
        document["asset"] = {{"version", "2.0"}, {"generator", "MiniEngine kn5 importer (assetto-corsa-gltf rules)"}};
        document["scene"] = 0;
        document["scenes"] = Json::array({Json{{"nodes", Json::array({rootNode})}}});
        document["nodes"] = m_nodes;
        document["meshes"] = m_meshes;
        document["materials"] = m_materials;
        document["accessors"] = m_accessors;
        document["bufferViews"] = m_bufferViews;
        document["buffers"] = Json::array({Json{{"uri", binaryUri}, {"byteLength", m_binary.size()}}});
        std::set<std::string> extensionsUsed = m_extensionsUsed;
        if (!m_vehicle.is_null())
        {
            document["extensions"][kVehicleExtension] = m_vehicle;
            extensionsUsed.insert(kVehicleExtension);
        }
        if (!extensionsUsed.empty())
        {
            document["extensionsUsed"] = std::vector<std::string>(extensionsUsed.begin(), extensionsUsed.end());
        }
        if (!m_images.empty())
        {
            document["images"] = m_images;
            document["textures"] = m_textures;
            // Linear, trilinear mipmaps, repeat.
            document["samplers"] = Json::array({Json{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
        }
        return document;
    }

    const std::vector<std::uint8_t>& Binary() const
    {
        return m_binary;
    }

    size_t NodeCount() const
    {
        return m_nodes.size();
    }

    size_t ImageCount() const
    {
        return m_images.size();
    }

  private:
    static bool IsDentMap(const std::string& textureName)
    {
        // On AC's damage shaders txNormal holds the dent map, blended in with accumulated damage:
        // zero on an undamaged car. Bound as a normal map it caves every panel in.
        return ToLowerAscii(textureName).find("damage") != std::string::npos;
    }

    // The maps a multilayer surface reads besides txDiffuse.
    static constexpr std::array<const char*, 6> kMultilayerSlots{
        "txMask", "txDetailR", "txDetailG", "txDetailB", "txDetailA", "txDetailNM"};

    // AC's ksMultilayer family, which blends detail maps by a mask (see AddDetailLayers).
    static bool IsMultilayer(const Kn5Material& material)
    {
        return ToLowerAscii(material.shader).starts_with("ksmultilayer") && !material.Texture("txMask").empty();
    }

    // The MASK cutoff of an alpha-tested material. ksAlphaRef is often 0 (or 0.01), and a cutoff
    // there passes every fragment.
    static float AlphaCutoff(const Kn5Material& material)
    {
        const float reference = material.Property("ksAlphaRef", 0.0f);
        return reference >= 0.02f ? reference : 0.5f;
    }

    // The lowest and highest alpha of a texture, decoded once per name; nullopt when it cannot be read.
    std::optional<std::pair<std::uint8_t, std::uint8_t>> AlphaRange(const Kn5Model& model, const std::string& textureName)
    {
        const std::string key = ToLowerAscii(textureName);
        const auto cached = m_alphaRangeCache.find(key);
        if (cached != m_alphaRangeCache.end())
        {
            return cached->second;
        }
        std::optional<std::pair<std::uint8_t, std::uint8_t>> range;
        const Kn5Texture* texture = FindTexture(model, textureName);
        if (texture != nullptr && texture->data.empty() && texture->size > 0)
        {
            return std::nullopt; // read without its texels: not known yet
        }
        if (texture != nullptr && texture->data.size() >= kStubTextureBytes)
        {
            if (const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name))
            {
                std::uint8_t low = 255;
                std::uint8_t high = 0;
                for (size_t pixel = 0; pixel * 4 + 3 < image->pixels.size(); ++pixel)
                {
                    low = std::min(low, image->pixels[pixel * 4 + 3]);
                    high = std::max(high, image->pixels[pixel * 4 + 3]);
                }
                range = std::make_pair(low, high);
            }
        }
        m_alphaRangeCache[key] = range;
        return range;
    }

    // Whether an alpha-tested material's test keeps anything. On ksPerPixelMultiMap the diffuse
    // alpha is the detail mask, and the Skyline's leather and headliner (_AT_NMDetail) sample a
    // diffuse whose alpha is 0 everywhere: as a cutout they would vanish, while the game draws them.
    bool AlphaTestCutsOut(const Kn5Model& model, const Kn5Material& material)
    {
        if (!material.alphaTested)
        {
            return false;
        }
        const std::optional<std::pair<std::uint8_t, std::uint8_t>> range = AlphaRange(model, material.Texture("txDiffuse"));
        return !range.has_value() || static_cast<float>(range->second) / 255.0f >= AlphaCutoff(material);
    }

    // ksPerPixelMultiMap's txDetail when it is a pattern (cloth weave, leather grain, carbon) and not
    // a flat colour, which DetailColor folds into the base colour instead.
    bool HasTiledDetail(const Kn5Model& model, const Kn5Material& material)
    {
        if (IsMultilayer(material) || material.Property("useDetail", 0.0f) <= 0.0f)
        {
            return false;
        }
        const std::string detail = material.Texture("txDetail");
        const Kn5Texture* texture = detail.empty() ? nullptr : FindTexture(model, detail);
        return texture != nullptr && texture->data.size() >= kStubTextureBytes && !DetailColor(model, detail).has_value();
    }

    // Decodes one texture and writes it as PNG under m_textureDirectory / fileName; false, with a
    // warning, when it cannot be decoded. Reads only state that is fixed by now, so texture
    // writers may run concurrently.
    bool WriteTexture(const Kn5Texture& texture, const std::string& key, const std::string& fileName) const
    {
        std::optional<TextureData> image = DecodeTextureBlob(texture.data, texture.name);
        if (!image.has_value())
        {
            return false;
        }

        bool alpha = m_alphaTextures.count(key) != 0;
        if (alpha)
        {
            alpha = false;
            for (size_t index = 3; index < image->pixels.size(); index += 4)
            {
                if (image->pixels[index] != 255)
                {
                    alpha = true;
                    break;
                }
            }
        }
        const int channels = alpha ? 4 : 3;
        WritePng(m_textureDirectory / fileName, image->width, image->height, channels,
                 alpha ? image->pixels.data() : StripAlpha(*image).data());
        return true;
    }

    static std::vector<std::uint8_t> StripAlpha(const TextureData& image)
    {
        std::vector<std::uint8_t> rgb;
        rgb.reserve(image.pixels.size() / 4 * 3);
        for (size_t index = 0; index < image.pixels.size(); index += 4)
        {
            rgb.push_back(image.pixels[index]);
            rgb.push_back(image.pixels[index + 1]);
            rgb.push_back(image.pixels[index + 2]);
        }
        return rgb;
    }

    std::string UniqueFileName(const std::string& stem, const std::string& extension)
    {
        std::string candidate = stem + extension;
        for (int suffix = 1; m_fileNames.count(ToLowerAscii(candidate)) != 0 ||
                             std::filesystem::exists(m_textureDirectory / candidate);
             ++suffix)
        {
            candidate = stem + "_" + std::to_string(suffix) + extension;
        }
        m_fileNames.insert(ToLowerAscii(candidate));
        return candidate;
    }

    std::optional<size_t> TextureIndexForUri(const std::string& uri)
    {
        if (uri.empty())
        {
            return std::nullopt;
        }
        const auto found = m_textureIndices.find(uri);
        if (found != m_textureIndices.end())
        {
            return found->second;
        }
        m_images.push_back(Json{{"uri", uri}});
        m_textures.push_back(Json{{"source", m_images.size() - 1}, {"sampler", 0}});
        m_textureIndices[uri] = m_textures.size() - 1;
        return m_textures.size() - 1;
    }

    std::optional<size_t> TextureIndexForKn5(const std::string& textureName)
    {
        const auto found = m_textureUris.find(ToLowerAscii(textureName));
        return found == m_textureUris.end() ? std::nullopt : TextureIndexForUri(found->second);
    }

    // txDetail's colour when it is one flat colour, in gamma space (FlatDetailColor). On
    // Kunos road cars this is where the paint lives: the diffuse is a grey panel/AO template shared
    // by every livery and the shader multiplies the one-colour detail map over it.
    std::optional<std::array<float, 3>> DetailColor(const Kn5Model& model, const std::string& textureName)
    {
        const auto cached = m_tintCache.find(textureName);
        if (cached != m_tintCache.end())
        {
            return cached->second;
        }
        std::optional<std::array<float, 3>> color;
        const Kn5Texture* texture = FindTexture(model, textureName);
        if (texture != nullptr && texture->data.size() >= kStubTextureBytes)
        {
            if (const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name))
            {
                color = Kn5Importer::FlatDetailColor(image->pixels, image->width, image->height);
            }
        }
        m_tintCache[textureName] = color;
        return color;
    }

    // The paint on its template, baked: AC multiplies the diffuse by lerp(colour, 1, diffuse alpha)
    // and the whole by the diffuse gain (Kn5Importer::DiffuseGain), in gamma space, and nothing
    // clamps before the product. A factor cannot carry a product above 1 and ignores the alpha mask,
    // so the product is written as the base colour map, clamped only at the end. A colour of 1 bakes
    // the gain alone. keepAlpha keeps the diffuse's alpha for a material that blends or tests it.
    std::optional<std::string> BakePaint(
        const Kn5Model& model, const std::string& diffuseName, const std::array<float, 3>& color, float gain, bool keepAlpha)
    {
        const std::string key = ToLowerAscii(diffuseName) + "|" + std::to_string(std::lround(color[0] * 1000.0f)) + "," +
                                std::to_string(std::lround(color[1] * 1000.0f)) + "," +
                                std::to_string(std::lround(color[2] * 1000.0f)) + "|" + std::to_string(std::lround(gain * 1000.0f)) +
                                (keepAlpha ? "|a" : "");
        const auto cached = m_paintCache.find(key);
        if (cached != m_paintCache.end())
        {
            return cached->second;
        }
        m_paintCache[key] = std::nullopt;
        const Kn5Texture* texture = FindTexture(model, diffuseName);
        if (texture == nullptr || texture->data.size() < kStubTextureBytes)
        {
            return std::nullopt;
        }
        const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name);
        if (!image.has_value())
        {
            return std::nullopt;
        }
        const size_t channels = keepAlpha ? 4 : 3;
        const size_t texels = static_cast<size_t>(image->width) * static_cast<size_t>(image->height);
        std::vector<std::uint8_t> pixels(texels * channels);
        for (size_t texel = 0; texel < texels; ++texel)
        {
            const float alpha = static_cast<float>(image->pixels[texel * 4 + 3]) / 255.0f;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const float diffuse = static_cast<float>(image->pixels[texel * 4 + channel]) / 255.0f;
                const float multiplier = (color[channel] + (1.0f - color[channel]) * alpha) * gain;
                pixels[texel * channels + channel] =
                    static_cast<std::uint8_t>(std::lround(std::clamp(diffuse * multiplier, 0.0f, 1.0f) * 255.0f));
            }
            if (keepAlpha)
            {
                pixels[texel * 4 + 3] = image->pixels[texel * 4 + 3];
            }
        }
        const bool paint = color != std::array<float, 3>{1.0f, 1.0f, 1.0f};
        const std::string fileName = UniqueFileName(SafeStem(diffuseName) + (paint ? "_paint" : "_lit"), ".png");
        WritePng(m_textureDirectory / fileName, image->width, image->height, static_cast<int>(channels), pixels.data());
        m_paintCache[key] = "textures/" + fileName;
        return m_paintCache[key];
    }

    // What a ksPerPixelMultiMap's txMaps becomes (docs/design/2026-10-06-car-paint-correctness-design.md).
    // AC reads it as R = specular, G = gloss, B = reflection; each lobe's exponent is G x EXP + 1.
    struct MapsBakeRequest
    {
        // The base lobe's exponent, before the gloss.
        float baseExponent = 0.0f;
        // The channel that masks the base's specular: 0 (R) or 2 (B).
        size_t maskChannel = 0;
        // Car paint: the coat's exponent, before the gloss, and its weight per unit of reflection
        // mask (fresnelC / kCoatF0); no coat map when the weight is 0.
        float coatExponent = 0.0f;
        float coatWeight = 0.0f;
    };

    struct BakedMaps
    {
        // R 0, G the base's roughness, B 0 (metallic), A the specular mask: the material's
        // metallicRoughnessTexture and KHR_materials_specular's specularTexture.
        std::optional<std::string> base;
        // R the coat's weight, G its roughness: clearcoatTexture and clearcoatRoughnessTexture.
        std::optional<std::string> coat;
    };

    BakedMaps BakeMaps(const Kn5Model& model, const std::string& textureName, const MapsBakeRequest& request)
    {
        const std::string key = ToLowerAscii(textureName) + "|" + std::to_string(std::lround(request.baseExponent * 100.0f)) + "|" +
                                std::to_string(request.maskChannel) + "|" + std::to_string(std::lround(request.coatExponent * 100.0f)) +
                                "|" + std::to_string(std::lround(request.coatWeight * 10000.0f));
        const auto cached = m_bakeCache.find(key);
        if (cached != m_bakeCache.end())
        {
            return cached->second;
        }
        m_bakeCache[key] = BakedMaps{};
        const Kn5Texture* texture = FindTexture(model, textureName);
        if (texture == nullptr || texture->data.size() < kStubTextureBytes)
        {
            return {};
        }
        const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name);
        if (!image.has_value())
        {
            return {};
        }

        // Per channel level: the base's and the coat's roughness at that gloss (exponent G x EXP + 1,
        // as Kunos' shader raises N.H to it), the coat's weight at that reflection mask.
        std::array<std::uint8_t, 256> baseRoughness{};
        std::array<std::uint8_t, 256> coatRoughness{};
        std::array<std::uint8_t, 256> coatWeight{};
        for (size_t value = 0; value < 256; ++value)
        {
            const float level = static_cast<float>(value) / 255.0f;
            baseRoughness[value] = static_cast<std::uint8_t>(
                std::lround(Kn5Importer::SpecularExponentToRoughness(request.baseExponent * level + 1.0f) * 255.0f));
            coatRoughness[value] = static_cast<std::uint8_t>(
                std::lround(Kn5Importer::SpecularExponentToRoughness(request.coatExponent * level + 1.0f) * 255.0f));
            coatWeight[value] = static_cast<std::uint8_t>(std::lround(std::clamp(request.coatWeight * level, 0.0f, 1.0f) * 255.0f));
        }
        const size_t texels = static_cast<size_t>(image->width) * static_cast<size_t>(image->height);
        const bool hasCoat = request.coatWeight > 0.0f;
        std::vector<std::uint8_t> base(texels * 4, 0);
        std::vector<std::uint8_t> coat(hasCoat ? texels * 3 : 0, 0);
        for (size_t texel = 0; texel < texels; ++texel)
        {
            const std::uint8_t* maps = &image->pixels[texel * 4];
            base[texel * 4 + 1] = baseRoughness[maps[1]];
            base[texel * 4 + 3] = maps[request.maskChannel];
            if (hasCoat)
            {
                coat[texel * 3] = coatWeight[maps[2]];
                coat[texel * 3 + 1] = coatRoughness[maps[1]];
            }
        }
        BakedMaps baked;
        const std::string stem = SafeStem(textureName);
        const std::string baseName = UniqueFileName(
            stem + "_base" + std::to_string(std::lround(request.baseExponent)) + (request.maskChannel == 2 ? "b" : "r"), ".png");
        WritePng(m_textureDirectory / baseName, image->width, image->height, 4, base.data());
        baked.base = "textures/" + baseName;
        if (hasCoat)
        {
            const std::string coatName = UniqueFileName(stem + "_coat" + std::to_string(std::lround(request.coatExponent)), ".png");
            WritePng(m_textureDirectory / coatName, image->width, image->height, 3, coat.data());
            baked.coat = "textures/" + coatName;
        }
        m_bakeCache[key] = baked;
        return baked;
    }

    // A flat paint detail's alpha: AC multiplies the specular by it where the detail applies, and
    // on metallic liveries it is the flake noise (tiled by detailUVMultiplier; a solid colour's is
    // one value). mean is its average; texture, when it varies, a grey map of it, sRGB encoded for
    // KHR_materials_specular's specularColorTexture.
    struct DetailFlake
    {
        float mean = 1.0f;
        std::optional<std::string> texture;
    };

    DetailFlake BakeDetailFlake(const Kn5Model& model, const std::string& detailName)
    {
        const std::string key = ToLowerAscii(detailName);
        const auto cached = m_flakeCache.find(key);
        if (cached != m_flakeCache.end())
        {
            return cached->second;
        }
        DetailFlake flake;
        m_flakeCache[key] = flake;
        const Kn5Texture* texture = FindTexture(model, detailName);
        if (texture == nullptr || texture->data.size() < kStubTextureBytes)
        {
            return flake;
        }
        const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name);
        if (!image.has_value())
        {
            return flake;
        }
        const size_t texels = static_cast<size_t>(image->width) * static_cast<size_t>(image->height);
        if (texels == 0)
        {
            return flake;
        }
        std::uint8_t low = 255;
        std::uint8_t high = 0;
        double sum = 0.0;
        for (size_t texel = 0; texel < texels; ++texel)
        {
            const std::uint8_t alpha = image->pixels[texel * 4 + 3];
            low = std::min(low, alpha);
            high = std::max(high, alpha);
            sum += alpha;
        }
        flake.mean = Round(static_cast<float>(sum / static_cast<double>(texels) / 255.0), 4);
        if (high - low > 6)
        {
            std::vector<std::uint8_t> grey(texels * 3);
            for (size_t texel = 0; texel < texels; ++texel)
            {
                const std::uint8_t value = LinearToSrgb8(static_cast<float>(image->pixels[texel * 4 + 3]) / 255.0f);
                grey[texel * 3] = value;
                grey[texel * 3 + 1] = value;
                grey[texel * 3 + 2] = value;
            }
            const std::string fileName = UniqueFileName(SafeStem(detailName) + "_flake", ".png");
            WritePng(m_textureDirectory / fileName, image->width, image->height, 3, grey.data());
            flake.texture = "textures/" + fileName;
        }
        m_flakeCache[key] = flake;
        return flake;
    }

    Json ConvertMaterial(const Kn5Model& model, const Kn5Material& material)
    {
        // AC's lobes as GGX ones (docs/design/2026-10-06-car-paint-correctness-design.md): the
        // exponent is the lobe's width and becomes roughness; ksSpecular is its height, which a lobe
        // of that width spends as reflectance (KHR_materials_specular), not as width.
        const std::string shader = ToLowerAscii(material.shader);
        const bool multilayer = shader.find("multilayer") != std::string::npos;
        const float exponent = std::max(material.Property("ksSpecularEXP", 20.0f), 0.0f);
        const float specular = std::clamp(material.Property("ksSpecular", 1.0f), 0.0f, 1.0f);
        const float fresnelMax = material.Property("fresnelMaxLevel", 0.0f);
        // Car paint: isAdditive 2 on a multi-map shader (Content Manager's IsCarpaint). Its
        // reflection, Fresnel-weighted from fresnelC up to fresnelMaxLevel, is the lacquer over the
        // base coat and becomes KHR_materials_clearcoat; the base keeps the ksSpecular lobe. Every
        // other surface's reflection is its base's specular, capped by fresnelMaxLevel.
        const bool carPaint = shader.find("multimap") != std::string::npos &&
                              std::lround(material.Property("isAdditive", 0.0f)) == 2 && fresnelMax > 0.0f;
        const float coatWeight = carPaint ? std::max(material.Property("fresnelC", 0.0f), 0.0f) / kCoatF0 : 0.0f;
        // The coat's exponent: the sun lobe's, AC's sharp second lobe on painted panels (the base's
        // where a paint has none).
        const float sunExponent = material.Property("sunSpecularEXP", 0.0f);
        const float coatExponent = sunExponent > 0.0f ? sunExponent : exponent;

        const bool useDetail = material.Property("useDetail", 0.0f) > 0.0f;
        const std::string detailName = material.Texture("txDetail");
        const std::optional<std::array<float, 3>> paint = useDetail ? DetailColor(model, detailName) : std::nullopt;
        const bool tiledDetail = !multilayer && !paint.has_value() && HasTiledDetail(model, material);

        // How bright AC draws the diffuse (docs/design/2026-10-06-ac-light-scale-design.md), a gain in
        // gamma space on everything the diffuse term draws: the paint colour, a tiled detail, or the
        // diffuse itself. Tracks are not calibrated yet: they keep a detail's old x2 and no gain.
        const float gain = m_carLighting ? Kn5Importer::DiffuseGain(material.Property("ksDiffuse", 0.5f),
                                                                    material.Property("ksAmbient", 0.5f))
                                         : 1.0f;
        const float detailGain = m_carLighting ? gain : 2.0f;
        const bool keepAlpha = material.alphaBlend || AlphaTestCutsOut(model, material);

        Json pbr = Json::object();
        pbr["baseColorFactor"] = Json::array({1.0f, 1.0f, 1.0f, 1.0f});
        pbr["metallicFactor"] = 0.0f;
        const std::string diffuseName = material.Texture("txDiffuse");
        std::optional<size_t> baseColor = TextureIndexForKn5(diffuseName);
        if (paint.has_value())
        {
            // A colour above 1 after the gain, or a diffuse whose alpha keeps the template somewhere,
            // needs the product baked; otherwise the colour is the factor.
            const float brightest = std::max({(*paint)[0], (*paint)[1], (*paint)[2]}) * detailGain;
            const std::optional<std::pair<std::uint8_t, std::uint8_t>> alpha = AlphaRange(model, diffuseName);
            std::optional<std::string> baked;
            if (baseColor.has_value() && (brightest > 1.0f || (alpha.has_value() && alpha->second > 0)))
            {
                baked = BakePaint(model, diffuseName, *paint, detailGain, keepAlpha);
            }
            if (baked.has_value())
            {
                baseColor = TextureIndexForUri(*baked);
            }
            else
            {
                pbr["baseColorFactor"] = Json::array({Round(SrgbToLinear(std::min((*paint)[0] * detailGain, 1.0f)), 5),
                                                      Round(SrgbToLinear(std::min((*paint)[1] * detailGain, 1.0f)), 5),
                                                      Round(SrgbToLinear(std::min((*paint)[2] * detailGain, 1.0f)), 5),
                                                      1.0f});
            }
        }
        else if (!tiledDetail && !multilayer && gain != 1.0f)
        {
            // The diffuse alone: a gain above 1 on a map has to be baked (a factor stops at 1); below
            // it, the factor in linear terms, gain^2.2.
            std::optional<std::string> baked;
            if (baseColor.has_value() && gain > 1.0f)
            {
                baked = BakePaint(model, diffuseName, {1.0f, 1.0f, 1.0f}, gain, keepAlpha);
            }
            if (baked.has_value())
            {
                baseColor = TextureIndexForUri(*baked);
            }
            else
            {
                const float level = Round(baseColor.has_value() ? std::pow(std::min(gain, 1.0f), 2.2f)
                                                                : SrgbToLinear(std::min(gain, 1.0f)),
                                          5);
                pbr["baseColorFactor"] = Json::array({level, level, level, 1.0f});
            }
        }
        if (baseColor.has_value())
        {
            pbr["baseColorTexture"] = {{"index", *baseColor}};
        }

        float roughness = Kn5Importer::SpecularExponentToRoughness(exponent);
        if (multilayer)
        {
            // Ground is dry and rough at any scale a lobe can show. AC's exponent is a broad Blinn
            // lobe plus a faint Fresnel sheen (tarmacSpecularMultiplier, which the shader spends on
            // the sheen's intensity): converted alone it leaves tarmac glossier than the game shows.
            roughness = std::max(roughness, kMultilayerMinRoughness);
        }
        pbr["roughnessFactor"] = Round(roughness, 4);

        // The base's specular level: ksSpecular on car paint and on surfaces without a reflection,
        // fresnelMaxLevel (which caps the reflection) on the others; txMaps masks it per pixel with
        // the matching channel, R (specular) or B (reflection).
        const bool reflective = !carPaint && fresnelMax > 0.0f;
        const float specularLevel = reflective ? std::min(fresnelMax, 1.0f) : specular;
        MapsBakeRequest request;
        request.baseExponent = exponent;
        request.maskChannel = reflective ? 2 : 0;
        request.coatExponent = carPaint ? coatExponent : 0.0f;
        request.coatWeight = coatWeight;
        const std::string mapsName = material.Texture("txMaps");
        const BakedMaps maps = mapsName.empty() ? BakedMaps{} : BakeMaps(model, mapsName, request);
        if (maps.base.has_value())
        {
            // glTF multiplies factor and texture, so the per-pixel value takes over.
            pbr["metallicRoughnessTexture"] = {{"index", *TextureIndexForUri(*maps.base)}};
            pbr["roughnessFactor"] = 1.0f;
        }

        Json out = Json::object();
        out["name"] = material.name;
        out["pbrMetallicRoughness"] = std::move(pbr);
        out["doubleSided"] = false;

        // KHR_materials_specular: F0 = 0.04 x colour x level, F90 = level. Uncapped, near-black trim
        // and glass render as nothing but sky.
        Json specularExtension = Json::object();
        if (specularLevel < 1.0f)
        {
            specularExtension["specularFactor"] = Round(specularLevel, 4);
        }
        if (maps.base.has_value())
        {
            specularExtension["specularTexture"] = {{"index", *TextureIndexForUri(*maps.base)}};
        }
        if (carPaint && paint.has_value())
        {
            // The paint detail's alpha scales the specular where it applies: a metallic livery's
            // flakes, tiled like the detail. As the colour, it weighs F0 only.
            const DetailFlake flake = BakeDetailFlake(model, detailName);
            const float tiling = material.Property("detailUVMultiplier", 1.0f);
            if (flake.texture.has_value() && tiling > 0.0f)
            {
                specularExtension["specularColorTexture"] = {
                    {"index", *TextureIndexForUri(*flake.texture)},
                    {"extensions", {{"KHR_texture_transform", {{"scale", {Round(tiling, 6), Round(tiling, 6)}}}}}}};
                m_extensionsUsed.insert("KHR_texture_transform");
            }
            else if (flake.mean < 1.0f)
            {
                specularExtension["specularColorFactor"] = {flake.mean, flake.mean, flake.mean};
            }
        }
        if (!specularExtension.empty())
        {
            out["extensions"]["KHR_materials_specular"] = std::move(specularExtension);
            m_extensionsUsed.insert("KHR_materials_specular");
        }

        // Car paint's lacquer: AC's reflection is fresnelC at normal incidence times txMaps' B, laid
        // over the lit paint (lerp by the Fresnel, isAdditive 2), the coat's 0.04 times its weight,
        // so the weight is fresnelC x B / 0.04; its roughness is the sun lobe's exponent with the
        // gloss. AC's own reflection of paint is mirror sharp: one lobe cannot be both, and the
        // physical conversion of the sun lobe is the one kept.
        if (carPaint && coatWeight > 0.0f)
        {
            Json coat = Json::object();
            if (maps.coat.has_value())
            {
                const size_t coatIndex = *TextureIndexForUri(*maps.coat);
                coat = {{"clearcoatFactor", 1.0f},
                        {"clearcoatTexture", {{"index", coatIndex}}},
                        {"clearcoatRoughnessFactor", 1.0f},
                        {"clearcoatRoughnessTexture", {{"index", coatIndex}}}};
            }
            else
            {
                coat = {{"clearcoatFactor", Round(std::min(coatWeight, 1.0f), 4)},
                        {"clearcoatRoughnessFactor", Round(Kn5Importer::SpecularExponentToRoughness(coatExponent), 4)}};
            }
            out["extensions"]["KHR_materials_clearcoat"] = std::move(coat);
            m_extensionsUsed.insert("KHR_materials_clearcoat");
        }

        const std::string normalName = material.Texture("txNormal");
        if (!IsDentMap(normalName))
        {
            if (const std::optional<size_t> normal = TextureIndexForKn5(normalName))
            {
                out["normalTexture"] = {{"index", *normal}};
            }
        }
        if (IsMultilayer(material))
        {
            AddDetailLayers(material, out);
        }
        else if (tiledDetail)
        {
            AddTiledDetail(model, material, out, detailGain);
        }

        if (material.alphaBlend)
        {
            out["alphaMode"] = "BLEND";
        }
        else if (AlphaTestCutsOut(model, material))
        {
            out["alphaMode"] = "MASK";
            out["alphaCutoff"] = AlphaCutoff(material);
        }

        if (m_carLighting && !material.alphaBlend && fresnelMax > 0.0f)
        {
            AddMetalReflector(material, exponent, gain, out);
        }
        AddEmissive(material, diffuseName, out);
        return out;
    }

    // AC lays the cube map over the lit surface by its Fresnel weight (isAdditive 0 and 2) or adds it
    // (1), all in gamma space. Where that reflection outweighs the diffuse the surface keeps, it is a
    // mirror-like metal: the headlight reflectors (ksDiffuse 0.01, the weight at its 0.7 cap a few
    // degrees off normal) and the mirrors. A dielectric's specular cannot reach that, so it becomes
    // metal: grey, at the reflection's mean in linear terms, with no colour map (AC's reflection is
    // the untinted cube map), and as sharp as AC's reflection (its cube-map level of detail is 0).
    void AddMetalReflector(const Kn5Material& material, float exponent, float gain, Json& out)
    {
        const int additive = static_cast<int>(std::lround(material.Property("isAdditive", 0.0f)));
        const float reflection =
            Kn5Importer::MeanReflection(material.Property("fresnelC", 0.0f), material.Property("fresnelEXP", 0.0f),
                                        material.Property("fresnelMaxLevel", 0.0f), additive);
        if (reflection <= gain)
        {
            return;
        }
        // The shader's cube-map level, at full gloss: 6 x (1 - EXP / 8) on isAdditive 2, 6 x (1 - EXP / 255)
        // otherwise. Level 0 is the sharp map.
        const float level = 6.0f * std::clamp(1.0f - exponent / (additive == 2 ? 8.0f : 255.0f), 0.0f, 1.0f);
        const float roughness = level <= 0.0f ? kMinRoughness : Kn5Importer::SpecularExponentToRoughness(exponent);
        const float reflectance = Round(std::pow(std::min(reflection, 1.0f), 2.2f), 4);
        Json& pbr = out["pbrMetallicRoughness"];
        pbr["metallicFactor"] = 1.0f;
        pbr["baseColorFactor"] = Json::array({reflectance, reflectance, reflectance, 1.0f});
        pbr["roughnessFactor"] = Round(roughness, 4);
        pbr.erase("baseColorTexture");
        pbr.erase("metallicRoughnessTexture");
    }

    // ksEmissive is a colour (the kn5's valueC): AC adds diffuse x ksEmissive to the lit term, in its
    // light units, in gamma space. A neutral lit surface is kAcNeutralLight of them, drawn at
    // kAcNeutralGain; made linear, the emission is a white surface's daylight luminance here times
    // (ksEmissive x kAcNeutralGain / kAcNeutralLight)^2.2.
    void AddEmissive(const Kn5Material& material, const std::string& diffuseName, Json& out)
    {
        std::array<float, 3> color{};
        const auto vector = material.vectors.find("ksEmissive");
        if (vector != material.vectors.end())
        {
            color = vector->second;
        }
        const float scalar = material.Property("ksEmissive", 0.0f);
        float peak = 0.0f;
        for (float& channel : color)
        {
            channel = std::pow(std::max(std::max(channel, scalar), 0.0f) * kAcNeutralGain / kAcNeutralLight, 2.2f);
            peak = std::max(peak, channel);
        }
        if (peak <= 0.0f)
        {
            return;
        }
        out["emissiveFactor"] = {Round(color[0] / peak, 4), Round(color[1] / peak, 4), Round(color[2] / peak, 4)};
        if (const std::optional<size_t> diffuse = TextureIndexForKn5(diffuseName))
        {
            out["emissiveTexture"] = {{"index", *diffuse}};
        }
        out["extensions"]["KHR_materials_emissive_strength"] = {{"emissiveStrength", Round(peak * kAcNeutralLuminance, 1)}};
        m_extensionsUsed.insert("KHR_materials_emissive_strength");
    }

    // ksPerPixelMultiMap's tiled detail (docs/design/2026-10-06-multimap-detail-design.md), as one
    // MINIENGINE_materials_detail_layers layer: AC multiplies the diffuse by the detail where the
    // diffuse's alpha is 0, and leaves it alone where the alpha is 1; the layers' intensity is the
    // diffuse gain on both. The mask carries 1 - alpha in red for the detail and alpha in green for a
    // neutral layer. The _NMDetail shaders' txNormalDetail, tiled the same way and scaled by
    // detailNormalBlend, becomes the normal map where the material's own is flat.
    void AddTiledDetail(const Kn5Model& model, const Kn5Material& material, Json& out, float intensity)
    {
        const std::optional<size_t> detail = TextureIndexForKn5(material.Texture("txDetail"));
        const std::optional<DetailMask> mask = BakeDetailMask(model, material.Texture("txDiffuse"));
        if (!detail.has_value() || !mask.has_value())
        {
            return;
        }
        const float tiling = Round(material.Property("detailUVMultiplier", 1.0f), 6);
        Json layers = Json::array();
        layers.push_back(Json{{"texture", {{"index", *detail}}}, {"scale", {tiling, tiling}}});
        if (mask->keepsDiffuse)
        {
            // Where the diffuse is kept only the gain applies: a white layer at a car's gain, a
            // mid-grey one under a track's x2.
            layers.push_back(Json{{"texture", {{"index", NeutralDetailIndex(m_carLighting)}}}, {"scale", {1.0f, 1.0f}}});
        }
        out["extensions"]["MINIENGINE_materials_detail_layers"] = {
            {"maskTexture", {{"index", *TextureIndexForUri(mask->uri)}}},
            {"mapping", "texCoord"},
            {"intensity", Round(intensity, 5)},
            {"layers", std::move(layers)}};
        m_extensionsUsed.insert("MINIENGINE_materials_detail_layers");

        const float normalBlend = material.Property("detailNormalBlend", 0.0f);
        const std::string ownNormal = material.Texture("txNormal");
        const bool ownNormalFlat = !out.contains("normalTexture") || DetailColor(model, ownNormal).has_value();
        if (normalBlend > 0.0f && ownNormalFlat)
        {
            if (const std::optional<size_t> normal = TextureIndexForKn5(material.Texture("txNormalDetail")))
            {
                out["normalTexture"] = {
                    {"index", *normal},
                    {"scale", Round(normalBlend, 6)},
                    {"extensions", {{"KHR_texture_transform", {{"scale", {tiling, tiling}}}}}}};
                m_extensionsUsed.insert("KHR_texture_transform");
            }
        }
    }

    struct DetailMask
    {
        std::string uri;
        bool keepsDiffuse = false; // some texel's alpha is above 0: the mid-grey layer is needed
    };

    // The detail mask of a diffuse map (see AddTiledDetail), written once per diffuse: one texel
    // when the alpha is uniform, as it is on most cars.
    std::optional<DetailMask> BakeDetailMask(const Kn5Model& model, const std::string& diffuseName)
    {
        const std::string key = ToLowerAscii(diffuseName);
        const auto cached = m_detailMaskCache.find(key);
        if (cached != m_detailMaskCache.end())
        {
            return cached->second;
        }
        m_detailMaskCache[key] = std::nullopt;
        // No readable diffuse: AC's sampler returns 0 alpha, the detail everywhere.
        const std::pair<std::uint8_t, std::uint8_t> range =
            AlphaRange(model, diffuseName).value_or(std::make_pair<std::uint8_t, std::uint8_t>(0, 0));
        int width = 1;
        int height = 1;
        std::vector<std::uint8_t> rgba{static_cast<std::uint8_t>(255 - range.first), range.first, 0, 0};
        if (range.first != range.second)
        {
            const Kn5Texture* texture = FindTexture(model, diffuseName);
            const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name);
            if (!image.has_value())
            {
                return std::nullopt;
            }
            width = image->width;
            height = image->height;
            rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
            for (size_t pixel = 0; pixel < rgba.size() / 4; ++pixel)
            {
                const std::uint8_t alpha = image->pixels[pixel * 4 + 3];
                rgba[pixel * 4] = static_cast<std::uint8_t>(255 - alpha);
                rgba[pixel * 4 + 1] = alpha;
            }
        }
        const std::string fileName = UniqueFileName(SafeStem(diffuseName) + "_detail_mask", ".png");
        WritePng(m_textureDirectory / fileName, width, height, 4, rgba.data());
        m_detailMaskCache[key] = DetailMask{"textures/" + fileName, range.second > 0};
        return m_detailMaskCache[key];
    }

    // A one-texel map for the detail layer that leaves the diffuse as it is: white (times the gain)
    // or mid-grey (times a track's x2).
    size_t NeutralDetailIndex(bool white)
    {
        std::string& uri = white ? m_whiteDetailUri : m_neutralDetailUri;
        if (uri.empty())
        {
            const std::string fileName = UniqueFileName(white ? "detail_white" : "detail_neutral", ".png");
            const std::uint8_t level = white ? 255 : 128;
            const std::array<std::uint8_t, 3> texel{level, level, level};
            WritePng(m_textureDirectory / fileName, 1, 1, 3, texel.data());
            uri = "textures/" + fileName;
        }
        return *TextureIndexForUri(uri);
    }

    // AC's multilayer surfaces (see docs/design/2026-09-28-detail-layers-design.md): the mask and
    // the four details as MINIENGINE_materials_detail_layers, txDetailNM as a tiled normal map.
    void AddDetailLayers(const Kn5Material& material, Json& out)
    {
        const std::optional<size_t> mask = TextureIndexForKn5(material.Texture("txMask"));
        if (!mask.has_value())
        {
            return;
        }
        // ksMultilayer_objsp tiles by the mesh's UV, the others by the world's x and z. The model's
        // space is AC's world turned half about Y, so a world-position scale changes sign.
        const bool byTexCoord = ToLowerAscii(material.shader).starts_with("ksmultilayer_objsp");
        const float sign = byTexCoord ? 1.0f : -1.0f;
        Json layers = Json::array();
        constexpr std::array<std::pair<const char*, const char*>, 4> kLayers{
            {{"txDetailR", "multR"}, {"txDetailG", "multG"}, {"txDetailB", "multB"}, {"txDetailA", "multA"}}};
        for (const auto& [slot, multiplier] : kLayers)
        {
            const float scale = Round(sign * material.Property(multiplier, 1.0f), 6);
            Json layer = Json{{"scale", {scale, scale}}};
            if (const std::optional<size_t> texture = TextureIndexForKn5(material.Texture(slot)))
            {
                layer["texture"] = {{"index", *texture}};
            }
            layers.push_back(std::move(layer));
        }
        out["extensions"]["MINIENGINE_materials_detail_layers"] = {
            {"maskTexture", {{"index", *mask}}},
            {"mapping", byTexCoord ? "texCoord" : "positionXZ"},
            {"intensity", Round(material.Property("magicMult", 1.0f), 6)},
            {"layers", std::move(layers)}};
        m_extensionsUsed.insert("MINIENGINE_materials_detail_layers");

        // With detailNMMult 0 AC samples a single texel of the map: no relief worth binding.
        const float normalTiling = material.Property("detailNMMult", 0.0f);
        if (!out.contains("normalTexture") && normalTiling > 0.0f)
        {
            if (const std::optional<size_t> normal = TextureIndexForKn5(material.Texture("txDetailNM")))
            {
                out["normalTexture"] = {
                    {"index", *normal},
                    {"extensions", {{"KHR_texture_transform", {{"scale", {Round(normalTiling, 6), Round(normalTiling, 6)}}}}}}};
                m_extensionsUsed.insert("KHR_texture_transform");
            }
        }
    }

    size_t AppendView(const void* data, size_t size, int target)
    {
        while (m_binary.size() % 4 != 0)
        {
            m_binary.push_back(0);
        }
        const size_t offset = m_binary.size();
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_binary.insert(m_binary.end(), bytes, bytes + size);
        m_bufferViews.push_back(Json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}, {"target", target}});
        return m_bufferViews.size() - 1;
    }

    size_t AppendFloats(const std::vector<float>& values, size_t components, bool bounds)
    {
        const size_t view = AppendView(values.data(), values.size() * sizeof(float), kArrayBuffer);
        static const char* const kTypes[] = {"", "SCALAR", "VEC2", "VEC3", "VEC4"};
        Json accessor{{"bufferView", view}, {"componentType", kFloat}, {"count", values.size() / components}, {"type", kTypes[components]}};
        if (bounds)
        {
            std::vector<float> minimum(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(components));
            std::vector<float> maximum = minimum;
            for (size_t index = 0; index < values.size(); ++index)
            {
                minimum[index % components] = std::min(minimum[index % components], values[index]);
                maximum[index % components] = std::max(maximum[index % components], values[index]);
            }
            accessor["min"] = minimum;
            accessor["max"] = maximum;
        }
        m_accessors.push_back(std::move(accessor));
        return m_accessors.size() - 1;
    }

    // How many of the node's indices to use (a whole number of triangles) when every one lands on a
    // vertex; 0 for a mesh with none, or one that indexes past its vertices.
    static size_t UsableIndexCount(const Kn5Node& node)
    {
        const size_t vertexCount = node.vertices.size();
        const size_t indexCount = node.indices.size() - node.indices.size() % 3;
        if (indexCount == 0 || vertexCount == 0)
        {
            return 0;
        }
        for (size_t index = 0; index < indexCount; ++index)
        {
            if (node.indices[index] >= vertexCount)
            {
                return 0;
            }
        }
        return indexCount;
    }

    // A physics mesh: positions and indices only, and no material. Only the collision needs it.
    std::optional<size_t> EmitCollisionMesh(const Kn5Node& node)
    {
        const size_t indexCount = UsableIndexCount(node);
        if (indexCount == 0)
        {
            ++m_report.emptyMeshes;
            return std::nullopt;
        }

        std::vector<float> positions;
        positions.reserve(node.vertices.size() * 3);
        for (const Kn5Vertex& vertex : node.vertices)
        {
            std::array<float, 3> position = vertex.position;
            if (!IsFinite(position.data(), 3))
            {
                position = {0.0f, 0.0f, 0.0f};
                ++m_report.scrubbedAttributes;
            }
            positions.insert(positions.end(), position.begin(), position.end());
        }
        Json attributes = Json::object();
        attributes["POSITION"] = AppendFloats(positions, 3, true);

        const size_t indexView = AppendView(node.indices.data(), indexCount * sizeof(std::uint16_t), kElementArrayBuffer);
        m_accessors.push_back(Json{{"bufferView", indexView}, {"componentType", kUnsignedShort}, {"count", indexCount}, {"type", "SCALAR"}});

        Json primitive{{"attributes", std::move(attributes)}, {"indices", m_accessors.size() - 1}};
        m_meshes.push_back(Json{{"name", node.name}, {"primitives", Json::array({std::move(primitive)})}});
        ++m_report.collisionMeshes;
        m_report.collisionTriangles += indexCount / 3;
        return m_meshes.size() - 1;
    }

    std::optional<size_t> EmitMesh(const Kn5Node& node)
    {
        const size_t vertexCount = node.vertices.size();
        const size_t indexCount = UsableIndexCount(node);
        if (indexCount == 0)
        {
            if (node.indices.size() >= 3 && vertexCount > 0)
            {
                LOG_WARN("kn5 mesh '{}' indexes past its {} vertices; kept as an empty node", node.name, vertexCount);
            }
            ++m_report.emptyMeshes;
            return std::nullopt;
        }

        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<float> uvs;
        std::vector<float> tangents;
        positions.reserve(vertexCount * 3);
        normals.reserve(vertexCount * 3);
        uvs.reserve(vertexCount * 2);
        tangents.reserve(vertexCount * 4);
        for (const Kn5Vertex& vertex : node.vertices)
        {
            // Real files carry NaN (dash-light tangents, whole attributes): replace, and count.
            std::array<float, 3> position = vertex.position;
            if (!IsFinite(position.data(), 3))
            {
                position = {0.0f, 0.0f, 0.0f};
                ++m_report.scrubbedAttributes;
            }
            if (!IsFinite(vertex.normal.data(), 3))
            {
                ++m_report.scrubbedAttributes;
            }
            const std::array<float, 3> normal = Normalized(vertex.normal, {0.0f, 1.0f, 0.0f});
            std::array<float, 2> uv = vertex.uv;
            if (!IsFinite(uv.data(), 2))
            {
                uv = {0.0f, 0.0f};
                ++m_report.scrubbedAttributes;
            }
            if (m_options.flipUv)
            {
                uv[1] = -uv[1];
            }
            if (!IsFinite(vertex.tangent.data(), 3))
            {
                ++m_report.scrubbedAttributes;
            }
            const std::array<float, 3> tangent = Normalized(vertex.tangent, Perpendicular(normal));

            positions.insert(positions.end(), position.begin(), position.end());
            normals.insert(normals.end(), normal.begin(), normal.end());
            uvs.insert(uvs.end(), uv.begin(), uv.end());
            // kn5 tangents are vec3; the handedness it does not record is taken as +1.
            tangents.insert(tangents.end(), {tangent[0], tangent[1], tangent[2], 1.0f});
        }

        Json attributes = Json::object();
        attributes["POSITION"] = AppendFloats(positions, 3, true);
        attributes["NORMAL"] = AppendFloats(normals, 3, false);
        attributes["TEXCOORD_0"] = AppendFloats(uvs, 2, false);
        attributes["TANGENT"] = AppendFloats(tangents, 4, false);

        const size_t indexView = AppendView(node.indices.data(), indexCount * sizeof(std::uint16_t), kElementArrayBuffer);
        m_accessors.push_back(Json{{"bufferView", indexView}, {"componentType", kUnsignedShort}, {"count", indexCount}, {"type", "SCALAR"}});

        Json primitive{{"attributes", std::move(attributes)}, {"indices", m_accessors.size() - 1}};
        // A mesh's material index is relative to its own kn5.
        if (node.materialIndex < m_materialCount)
        {
            primitive["material"] = m_materialBase + node.materialIndex;
        }
        m_meshes.push_back(Json{{"name", node.name}, {"primitives", Json::array({std::move(primitive)})}});
        ++m_report.meshes;
        m_report.triangles += indexCount / 3;
        return m_meshes.size() - 1;
    }

    std::filesystem::path m_textureDirectory;
    Kn5ImportOptions m_options;
    std::vector<Kn5Surface> m_surfaces;
    Kn5ImportReport m_report;
    std::set<std::string> m_lowRes;
    size_t m_materialBase = 0;
    size_t m_materialCount = 0;
    std::vector<std::uint8_t> m_binary;
    Json m_nodes = Json::array();
    Json m_meshes = Json::array();
    Json m_materials = Json::array();
    Json m_accessors = Json::array();
    Json m_bufferViews = Json::array();
    Json m_images = Json::array();
    Json m_textures = Json::array();
    std::set<std::string> m_extensionsUsed;
    Json m_vehicle;
    std::unordered_set<std::string> m_fileNames;
    // Keyed by lower-case texture name.
    std::unordered_set<std::string> m_usedTextures;
    std::unordered_set<std::string> m_alphaTextures;
    // Of m_usedTextures, those some material samples as txDiffuse or txNormal, and those a
    // multilayer one samples: the rest are ksPerPixelMultiMap details only.
    std::unordered_set<std::string> m_plainTextures;
    std::unordered_set<std::string> m_multilayerTextures;
    std::unordered_map<std::string, std::string> m_textureUris;
    std::unordered_map<std::string, size_t> m_textureIndices;
    std::unordered_map<std::string, std::optional<std::array<float, 3>>> m_tintCache;
    std::unordered_map<std::string, BakedMaps> m_bakeCache;
    std::unordered_map<std::string, std::optional<std::string>> m_paintCache;
    std::unordered_map<std::string, DetailFlake> m_flakeCache;
    std::unordered_map<std::string, std::optional<std::pair<std::uint8_t, std::uint8_t>>> m_alphaRangeCache;
    std::unordered_map<std::string, std::optional<DetailMask>> m_detailMaskCache;
    std::string m_neutralDetailUri;
    std::string m_whiteDetailUri;
    // A car's import: its materials take AC's diffuse gain, metal reflectors (see ConvertMaterial).
    bool m_carLighting = false;
};

void CollectNodeNames(const Kn5Node& node, std::vector<std::string>& names)
{
    names.push_back(node.name);
    for (const Kn5Node& child : node.children)
    {
        CollectNodeNames(child, names);
    }
}

// The colour a texture is everywhere, as each channel's middle in [0, 1]: nullopt when any channel
// spans more than 6 levels. Extrema over every texel, not a downsample: averaging first would
// flatten leather grain and brushed metal to a constant too.
std::optional<std::array<float, 3>> FlatColorMiddle(const std::vector<std::uint8_t>& rgba, int width, int height)
{
    const size_t texels = static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0));
    if (texels == 0 || rgba.size() < texels * 4)
    {
        return std::nullopt;
    }
    std::array<int, 3> low{255, 255, 255};
    std::array<int, 3> high{0, 0, 0};
    for (size_t texel = 0; texel < texels; ++texel)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const int value = rgba[texel * 4 + static_cast<size_t>(channel)];
            low[channel] = std::min(low[channel], value);
            high[channel] = std::max(high[channel], value);
        }
    }
    std::array<float, 3> middle{};
    for (int channel = 0; channel < 3; ++channel)
    {
        if (high[channel] - low[channel] > 6)
        {
            return std::nullopt;
        }
        middle[channel] = static_cast<float>(low[channel] + high[channel]) * 0.5f / 255.0f;
    }
    return middle;
}

// Meshes, triangles (in total and per material) and the subtrees a default import drops.
void SurveyNodes(
    const Kn5Node& node,
    const std::set<std::string>& lowRes,
    bool insideDropped,
    Kn5ModelSummary& summary,
    std::vector<size_t>& materialTriangles)
{
    bool dropped = insideDropped;
    if (!insideDropped && (Kn5Importer::IsRuntimeVariant(node.name) || lowRes.count(node.name) != 0))
    {
        ++summary.runtimeVariants;
        dropped = true;
    }
    if (node.HasGeometry() && (!node.renderable || Kn5Importer::IsTrackMarker(node.name)))
    {
        ++summary.hiddenMeshes;
    }
    else if (node.HasGeometry())
    {
        ++summary.meshes;
        summary.triangles += node.triangleCount;
        if (node.materialIndex < materialTriangles.size())
        {
            materialTriangles[node.materialIndex] += node.triangleCount;
        }
    }
    for (const Kn5Node& child : node.children)
    {
        SurveyNodes(child, lowRes, dropped, summary, materialTriangles);
    }
}
}

namespace Kn5Importer
{
bool IsKn5Path(const std::filesystem::path& path)
{
    return ToLowerAscii(path.extension().string()) == ".kn5";
}

std::vector<std::string> ListSkins(const std::filesystem::path& kn5Path)
{
    std::vector<std::string> skins;
    const std::filesystem::path root = kn5Path.parent_path() / "skins";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code dirEc;
        if (it->is_directory(dirEc))
        {
            skins.push_back(it->path().filename().string());
        }
    }
    std::sort(skins.begin(), skins.end());
    return skins;
}

bool IsRuntimeVariant(const std::string& nodeName)
{
    const std::string lower = ToLowerAscii(nodeName);
    return lower.find("blur") != std::string::npos || lower.find("damage") != std::string::npos;
}

bool IsTrackMarker(const std::string& nodeName)
{
    const std::string lower = ToLowerAscii(nodeName);
    const auto digitsFrom = [&lower](size_t start, size_t end)
    {
        return end > start && std::all_of(lower.begin() + static_cast<std::ptrdiff_t>(start),
                                          lower.begin() + static_cast<std::ptrdiff_t>(end),
                                          [](char c) { return c >= '0' && c <= '9'; });
    };
    for (const std::string_view prefix : {"ac_start_", "ac_pit_", "ac_hotlap_start_"})
    {
        if (lower.starts_with(prefix) && digitsFrom(prefix.size(), lower.size()))
        {
            return true;
        }
    }
    constexpr std::string_view kTime = "ac_time_";
    return lower.starts_with(kTime) && lower.size() > kTime.size() + 2 &&
           (lower.ends_with("_l") || lower.ends_with("_r")) && digitsFrom(kTime.size(), lower.size() - 2);
}

std::set<std::string> LowResTwins(const std::vector<std::string>& nodeNames)
{
    std::unordered_set<std::string> present;
    for (const std::string& name : nodeNames)
    {
        present.insert(ToLowerAscii(name));
    }
    std::set<std::string> twins;
    for (const std::string& name : nodeNames)
    {
        const std::string lower = ToLowerAscii(name);
        if (EndsWith(lower, "_lr") && present.count(lower.substr(0, lower.size() - 3) + "_hr") != 0)
        {
            twins.insert(name);
        }
    }
    return twins;
}

float SpecularExponentToRoughness(float exponent)
{
    // Walter et al. 2007: a Blinn-Phong exponent n has Beckmann's (and near enough GGX's) alpha
    // sqrt(2 / (n + 2)). glTF's roughness is perceptual, alpha = roughness^2.
    return std::clamp(std::pow(2.0f / (std::max(exponent, 0.0f) + 2.0f), 0.25f), 0.04f, 1.0f);
}

std::optional<std::array<float, 3>> FlatDetailColor(const std::vector<std::uint8_t>& rgba, int width, int height)
{
    const std::optional<std::array<float, 3>> middle = FlatColorMiddle(rgba, width, height);
    if (!middle.has_value())
    {
        return std::nullopt;
    }
    std::array<float, 3> color{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        color[channel] = Round((*middle)[channel], 5);
    }
    return color;
}

float DiffuseGain(float ksDiffuse, float ksAmbient)
{
    return kAcNeutralGain * std::max(ksDiffuse * kAcSunLight + ksAmbient * kAcAmbientLight, 0.0f) / kAcNeutralLight;
}

float MeanReflection(float fresnelC, float fresnelExp, float fresnelMax, int isAdditive)
{
    // 2 x the integral of F(mu) mu over mu = N.V in (0, 1], by the midpoint rule; F jumps to its
    // cap within a few degrees for a small exponent, so the steps are fine.
    const float exponent = isAdditive == 0 ? std::max(fresnelExp, 1.0f) : std::max(fresnelExp, 0.0f);
    constexpr int kSteps = 4096;
    double sum = 0.0;
    for (int step = 0; step < kSteps; ++step)
    {
        const float mu = (static_cast<float>(step) + 0.5f) / static_cast<float>(kSteps);
        const float weight = std::min(fresnelC + std::pow(1.0f - mu, exponent), fresnelMax);
        sum += 2.0 * std::max(weight, 0.0f) * mu;
    }
    return static_cast<float>(sum / kSteps);
}

std::vector<size_t> RankPaintedMaterials(
    const std::vector<std::string>& materialNames,
    const std::vector<bool>& painted,
    const std::vector<size_t>& triangles)
{
    // What a car is painted beside, which is the half that matters: a rim is a painted material
    // on most Kunos cars and can carry more triangles than the bodywork.
    static const std::regex kNotBody(
        "rim|wheel|tyre|tire|brake|calip|disc|glass|window|light|lamp|badge|logo|plate|mirror|seat|"
        "interior|cockpit|dash|carpet|leather|belt|driver|steer|engine|exhaust|grill|plastic|chrome|"
        "rubber|shadow");
    static const std::regex kPaintHint("car[_ ]?paint|(^|[^a-z])body([^a-z]|$)|chassis");

    struct Ranked
    {
        size_t index;
        int band;
        int inside;
        size_t triangles;
    };
    std::vector<Ranked> ranked;
    for (size_t index = 0; index < materialNames.size() && index < painted.size(); ++index)
    {
        if (!painted[index])
        {
            continue;
        }
        const std::string lower = ToLowerAscii(materialNames[index]);
        // An interior copy of the paint (INT_OCC_Carpaint) is the same colour inside the panels.
        const int inside = lower.rfind("int", 0) == 0 ? 1 : 0;
        const int band = std::regex_search(lower, kNotBody) ? 2 : (std::regex_search(lower, kPaintHint) ? 0 : 1);
        ranked.push_back({index, band, inside, index < triangles.size() ? triangles[index] : 0});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b)
                     {
                         if (a.band != b.band)
                         {
                             return a.band < b.band;
                         }
                         if (a.inside != b.inside)
                         {
                             return a.inside < b.inside;
                         }
                         return a.triangles > b.triangles;
                     });
    std::vector<size_t> order;
    order.reserve(ranked.size());
    for (const Ranked& entry : ranked)
    {
        order.push_back(entry.index);
    }
    return order;
}

}

namespace
{
// AC's ini: [SECTION] then KEY=VALUE, keys upper-cased. Hand-rolled because the files have
// duplicate keys, empty values and '%' in values.
using AcIni = std::vector<std::pair<std::string, std::map<std::string, std::string>>>;

AcIni ParseAcIni(std::istream& file)
{
    const auto trim = [](std::string text)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        const size_t last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    AcIni sections;
    std::string line;
    while (std::getline(file, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line.rfind("//", 0) == 0)
        {
            continue;
        }
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            sections.emplace_back(line.substr(1, close == std::string::npos ? std::string::npos : close - 1), std::map<std::string, std::string>{});
            continue;
        }
        const size_t equals = line.find('=');
        if (!sections.empty() && equals != std::string::npos)
        {
            std::string key = trim(line.substr(0, equals));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
                           {
                               return static_cast<char>(std::toupper(character));
                           });
            sections.back().second[key] = trim(line.substr(equals + 1));
        }
    }
    return sections;
}

AcIni ReadAcIni(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    return ParseAcIni(file);
}

// "x, y, z" as three floats; zero for a missing or malformed value, as the game reads it.
std::array<float, 3> ParseTriple(const std::map<std::string, std::string>& values, const std::string& key)
{
    const auto found = values.find(key);
    if (found == values.end())
    {
        return {0.0f, 0.0f, 0.0f};
    }
    std::array<float, 3> triple{0.0f, 0.0f, 0.0f};
    std::stringstream stream(found->second);
    std::string part;
    for (size_t index = 0; index < 3 && std::getline(stream, part, ','); ++index)
    {
        try
        {
            size_t used = 0;
            triple[index] = std::stof(part, &used);
            if (part.find_first_not_of(" \t", used) != std::string::npos || !std::isfinite(triple[index]))
            {
                return {0.0f, 0.0f, 0.0f};
            }
        }
        catch (const std::exception&)
        {
            return {0.0f, 0.0f, 0.0f};
        }
    }
    return triple;
}

// The models an import converts: the kn5 itself, or a layout's.
std::vector<Kn5LayoutModel> ImportSources(const std::filesystem::path& source)
{
    if (Kn5Importer::IsLayoutPath(source))
    {
        return Kn5Importer::ReadLayout(source);
    }
    return {Kn5LayoutModel{source, {}, {}}};
}

// Counts one kn5's meshes, materials, textures and droppable variants into `summary`. Returns the
// triangles per material, for ranking its paint.
std::vector<size_t> SurveyModel(const Kn5Model& model, Kn5ModelSummary& summary)
{
    summary.materials += model.materials.size();
    summary.textures += model.textures.size();
    ++summary.models;
    std::vector<std::string> names;
    CollectNodeNames(model.root, names);
    std::vector<size_t> materialTriangles(model.materials.size(), 0);
    SurveyNodes(model.root, Kn5Importer::LowResTwins(names), false, summary, materialTriangles);
    return materialTriangles;
}

// Each livery beside a car, with the colour it paints the bodywork, then the embedded textures.
void SurveySkins(const std::filesystem::path& kn5Path, const Kn5Model& model, const std::vector<size_t>& materialTriangles, Kn5ModelSummary& summary)
{
    std::vector<std::string> materialNames;
    std::vector<bool> painted;
    for (const Kn5Material& material : model.materials)
    {
        materialNames.push_back(material.name);
        painted.push_back(!material.Texture("txDetail").empty() && material.Property("useDetail", 0.0f) > 0.0f);
    }
    const std::vector<size_t> paintOrder = Kn5Importer::RankPaintedMaterials(materialNames, painted, materialTriangles);

    // Keyed by the file the colour comes from: several materials (and every livery that does not
    // ship the texture) share one detail map, so each is decoded once.
    std::unordered_map<std::string, std::optional<std::array<std::uint8_t, 3>>> colors;
    const auto colorOf = [&](const std::string& key, const std::function<std::vector<std::uint8_t>()>& read)
    {
        const auto cached = colors.find(key);
        if (cached != colors.end())
        {
            return cached->second;
        }
        std::optional<std::array<std::uint8_t, 3>> color;
        const std::vector<std::uint8_t> blob = read();
        if (blob.size() >= kStubTextureBytes)
        {
            if (const std::optional<TextureData> image = DecodeTextureBlob(blob, key))
            {
                if (const std::optional<std::array<float, 3>> middle =
                        FlatColorMiddle(image->pixels, image->width, image->height))
                {
                    color = std::array<std::uint8_t, 3>{
                        static_cast<std::uint8_t>(std::lround((*middle)[0] * 255.0f)),
                        static_cast<std::uint8_t>(std::lround((*middle)[1] * 255.0f)),
                        static_cast<std::uint8_t>(std::lround((*middle)[2] * 255.0f))};
                }
            }
        }
        colors[key] = color;
        return color;
    };

    std::vector<std::string> skinNames = Kn5Importer::ListSkins(kn5Path);
    skinNames.push_back(std::string());
    for (const std::string& skinName : skinNames)
    {
        Kn5SkinSummary skin;
        skin.name = skinName;
        std::unordered_map<std::string, std::filesystem::path> overrides;
        if (!skinName.empty())
        {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(kn5Path.parent_path() / "skins" / skinName, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code fileEc;
                if (it->is_regular_file(fileEc))
                {
                    overrides[ToLowerAscii(it->path().filename().string())] = it->path();
                }
            }
        }
        for (size_t materialIndex : paintOrder)
        {
            const std::string detail = model.materials[materialIndex].Texture("txDetail");
            const auto own = overrides.find(ToLowerAscii(detail));
            std::optional<std::array<std::uint8_t, 3>> color;
            const bool fromSkin = own != overrides.end();
            if (fromSkin)
            {
                const std::filesystem::path file = own->second;
                color = colorOf(file.string(), [&]()
                                {
                                    try
                                    {
                                        return ReadFileBytes(file);
                                    }
                                    catch (const std::exception&)
                                    {
                                        return std::vector<std::uint8_t>();
                                    }
                                });
            }
            else
            {
                color = colorOf("kn5:" + detail, [&]()
                                {
                                    const Kn5Texture* texture = FindTexture(model, detail);
                                    return texture != nullptr ? texture->data : std::vector<std::uint8_t>();
                                });
            }
            if (color.has_value())
            {
                // The first flat colour in body-first order is the car's colour.
                skin.paint = color;
                skin.paintMaterial = model.materials[materialIndex].name;
                skin.paintFromSkin = fromSkin;
                break;
            }
        }
        summary.skins.push_back(std::move(skin));
    }
}
}

namespace Kn5Importer
{
bool IsPhysicsMeshName(const std::string& nodeName)
{
    return !nodeName.empty() && nodeName.front() >= '0' && nodeName.front() <= '9';
}

std::vector<Kn5Surface> ParseSurfaces(const std::string& iniText)
{
    std::istringstream stream(iniText);
    std::vector<Kn5Surface> surfaces;
    for (const auto& [section, values] : ParseAcIni(stream))
    {
        const auto key = values.find("KEY");
        if (ToLowerAscii(section).rfind("surface", 0) != 0 || key == values.end() || key->second.empty())
        {
            continue;
        }
        Kn5Surface surface;
        surface.key = key->second;
        surface.friction = kUnknownSurfaceFriction;
        if (const auto friction = values.find("FRICTION"); friction != values.end())
        {
            try
            {
                const float parsed = std::stof(friction->second);
                if (std::isfinite(parsed) && parsed >= 0.0f)
                {
                    surface.friction = parsed;
                }
            }
            catch (const std::exception&)
            {
                // Malformed: the unknown-surface friction stands.
            }
        }
        surfaces.push_back(std::move(surface));
    }
    return surfaces;
}

Kn5Surface MatchSurface(const std::vector<Kn5Surface>& surfaces, const std::string& nodeName)
{
    const auto isDigit = [](char character)
    {
        return character >= '0' && character <= '9';
    };
    size_t start = 0;
    while (start < nodeName.size() && isDigit(nodeName[start]))
    {
        ++start;
    }
    const std::string name = ToLowerAscii(nodeName.substr(start));

    const Kn5Surface* best = nullptr;
    for (const Kn5Surface& surface : surfaces)
    {
        const std::string key = ToLowerAscii(surface.key);
        if (!key.empty() && name.rfind(key, 0) == 0 && (best == nullptr || key.size() > best->key.size()))
        {
            best = &surface;
        }
    }
    if (best != nullptr)
    {
        return *best;
    }

    size_t end = nodeName.size();
    while (end > start && isDigit(nodeName[end - 1]))
    {
        --end;
    }
    return Kn5Surface{nodeName.substr(start, end - start), kUnknownSurfaceFriction};
}

bool IsLayoutPath(const std::filesystem::path& path)
{
    const std::string name = ToLowerAscii(path.filename().string());
    return name == "models.ini" || (name.rfind("models_", 0) == 0 && EndsWith(name, ".ini"));
}

std::string ImportName(const std::filesystem::path& source)
{
    if (!IsLayoutPath(source))
    {
        return source.stem().string();
    }
    const std::string track = source.parent_path().filename().string();
    const std::string stem = source.stem().string();
    // "models_endurance.ini" -> "<track>_endurance"; plain "models.ini" -> "<track>".
    return stem.size() > 7 ? track + "_" + stem.substr(7) : track;
}

std::vector<Kn5Surface> LoadTrackSurfaces(const std::filesystem::path& source)
{
    const std::filesystem::path trackDirectory = source.parent_path();
    std::vector<Kn5Surface> surfaces;
    const auto append = [&surfaces](const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file)
        {
            return;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        for (Kn5Surface& surface : Kn5Importer::ParseSurfaces(text))
        {
            surfaces.push_back(std::move(surface));
        }
    };

    // A layout's own data folder first: models_<layout>.ini reads <track>/<layout>/data, which is
    // where multi-layout tracks (ks_nordschleife) keep their only surfaces.ini.
    if (IsLayoutPath(source))
    {
        const std::string stem = source.stem().string();
        if (stem.size() > 7)
        {
            append(trackDirectory / stem.substr(7) / "data" / "surfaces.ini");
        }
    }
    append(trackDirectory / "data" / "surfaces.ini");
    std::error_code ec;
    std::filesystem::path ancestor = std::filesystem::absolute(trackDirectory, ec);
    for (int level = 0; !ec && level < 6 && ancestor.has_parent_path() && ancestor.parent_path() != ancestor; ++level)
    {
        const std::filesystem::path system = ancestor / "system" / "data" / "surfaces.ini";
        if (std::filesystem::is_regular_file(system, ec))
        {
            append(system);
            break;
        }
        ancestor = ancestor.parent_path();
    }
    for (const Kn5Surface& builtin : {Kn5Surface{"ROAD", 1.0f}, Kn5Surface{"GRASS", 0.6f}, Kn5Surface{"KERB", 0.92f}, Kn5Surface{"SAND", 0.8f}})
    {
        surfaces.push_back(builtin);
    }
    return surfaces;
}

std::vector<Kn5LayoutModel> ReadLayout(const std::filesystem::path& layoutPath)
{
    std::vector<Kn5LayoutModel> models;
    std::vector<std::string> missing;
    for (const auto& [section, values] : ReadAcIni(layoutPath))
    {
        const auto file = values.find("FILE");
        if (ToLowerAscii(section).rfind("model", 0) != 0 || file == values.end() || file->second.empty())
        {
            continue;
        }
        Kn5LayoutModel model;
        model.file = layoutPath.parent_path() / std::filesystem::path(file->second).make_preferred();
        model.position = ParseTriple(values, "POSITION");
        model.rotationDegrees = ParseTriple(values, "ROTATION");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(model.file, ec))
        {
            missing.push_back(file->second);
        }
        models.push_back(std::move(model));
    }
    if (models.empty())
    {
        throw std::runtime_error("'" + layoutPath.string() + "' places no models (no [MODEL_n] section with a FILE)");
    }
    if (!missing.empty())
    {
        std::string list;
        for (const std::string& name : missing)
        {
            list += (list.empty() ? "" : ", ") + name;
        }
        throw std::runtime_error("'" + layoutPath.filename().string() + "' names models that are not there: " + list);
    }
    return models;
}

std::vector<std::filesystem::path> FindLayouts(const std::filesystem::path& kn5Path)
{
    std::vector<std::filesystem::path> layouts;
    std::error_code ec;
    const std::filesystem::path wanted = std::filesystem::absolute(kn5Path, ec).lexically_normal();
    for (std::filesystem::directory_iterator it(kn5Path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !IsLayoutPath(it->path()))
        {
            continue;
        }
        try
        {
            for (const Kn5LayoutModel& model : ReadLayout(it->path()))
            {
                std::error_code absoluteEc;
                const std::filesystem::path placed = std::filesystem::absolute(model.file, absoluteEc).lexically_normal();
                // The game resolves FILE ignoring case.
                if (ToLowerAscii(placed.generic_string()) == ToLowerAscii(wanted.generic_string()))
                {
                    layouts.push_back(it->path());
                    break;
                }
            }
        }
        catch (const std::exception&)
        {
            // A broken layout is not offered; importing it directly reports why.
        }
    }
    std::sort(layouts.begin(), layouts.end());
    return layouts;
}

std::array<float, 16> LayoutModelMatrix(const std::array<float, 3>& position, const std::array<float, 3>& rotationDegrees)
{
    constexpr float kRadiansPerDegree = 3.14159265358979323846f / 180.0f;
    const float cx = std::cos(rotationDegrees[0] * kRadiansPerDegree);
    const float sx = std::sin(rotationDegrees[0] * kRadiansPerDegree);
    const float cy = std::cos(rotationDegrees[1] * kRadiansPerDegree);
    const float sy = std::sin(rotationDegrees[1] * kRadiansPerDegree);
    const float cz = std::cos(rotationDegrees[2] * kRadiansPerDegree);
    const float sz = std::sin(rotationDegrees[2] * kRadiansPerDegree);
    // Rows of Rz * Ry * Rx.
    const float m[3][3] = {
        {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
        {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
        {-sy, cy * sx, cy * cx}};
    return {m[0][0], m[1][0], m[2][0], 0.0f,
            m[0][1], m[1][1], m[2][1], 0.0f,
            m[0][2], m[1][2], m[2][2], 0.0f,
            position[0], position[1], position[2], 1.0f};
}

Kn5ModelSummary Inspect(const std::filesystem::path& source)
{
    Kn5ModelSummary summary;
    if (IsLayoutPath(source))
    {
        for (const Kn5LayoutModel& layoutModel : ReadLayout(source))
        {
            summary.encrypted = summary.encrypted || Kn5Reader::IsEncrypted(layoutModel.file);
            Kn5Model model = Kn5Reader::Load(layoutModel.file, Kn5ReadScope::NoGeometry);
            FoldTextureCase(model);
            SurveyModel(model, summary);
        }
        // Tracks have no liveries: only their own textures.
        summary.skins.push_back(Kn5SkinSummary{});
        return summary;
    }

    summary.encrypted = Kn5Reader::IsEncrypted(source);
    Kn5Model model = Kn5Reader::Load(source, Kn5ReadScope::NoGeometry);
    FoldTextureCase(model);
    const std::vector<size_t> materialTriangles = SurveyModel(model, summary);
    for (const std::filesystem::path& layout : FindLayouts(source))
    {
        summary.layouts.push_back({layout, ReadLayout(layout).size()});
    }
    if (summary.encrypted)
    {
        // Its textures are decoys: no colour read from them would be the real one.
        return summary;
    }
    SurveySkins(source, model, materialTriangles, summary);
    return summary;
}

Kn5ImportReport ConvertToGltf(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory,
    const Kn5ImportOptions& options,
    const ImportProgressCallback& progress)
{
    // The passes' shares of the whole; the geometry pass and the final write take the rest.
    constexpr float kTablesShare = 0.05f;
    constexpr float kTexturesShare = 0.45f;
    constexpr float kGeometryShare = 0.40f;
    float lastReported = 0.0f;
    const auto reportProgress = [&](float fraction)
    {
        // Passes only ever move forward, whichever model or texture they are on.
        lastReported = std::max(lastReported, std::clamp(fraction, 0.0f, 1.0f));
        if (progress)
        {
            progress(lastReported);
        }
    };

    const std::vector<Kn5LayoutModel> sources = ImportSources(source);
    const bool layout = IsLayoutPath(source);
    // Refused before anything is written.
    for (const Kn5LayoutModel& placed : sources)
    {
        if (Kn5Reader::IsEncrypted(placed.file))
        {
            throw std::runtime_error(
                "Refusing '" + placed.file.filename().string() +
                "': it carries the CSP kn5 encryption trailer. Its textures and several meshes are decoys in the plain "
                "section, so the import would be wrong without looking it.");
        }
    }

    const std::string name = ImportName(source);
    const std::filesystem::path gltfPath = targetDirectory / (name + ".gltf");
    const std::filesystem::path binaryPath = targetDirectory / "buffers" / (name + ".bin");
    std::error_code existsEc;
    if (std::filesystem::exists(gltfPath, existsEc) || std::filesystem::exists(binaryPath, existsEc))
    {
        throw std::runtime_error("'" + gltfPath.string() + "' already exists; an import does not overwrite it");
    }

    const std::filesystem::path textureDirectory = targetDirectory / "textures";
    for (const std::filesystem::path& directory : {targetDirectory, textureDirectory, binaryPath.parent_path()})
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
        {
            throw std::runtime_error("Cannot create '" + directory.string() + "': " + ec.message());
        }
    }

    GltfBuilder builder(textureDirectory, options, Kn5Importer::LoadTrackSurfaces(source));
    Kn5ImportReport& report = builder.Report();
    // A car is a lone kn5 beside its data (data.acd, or an unpacked data/car.ini).
    std::error_code carEc;
    builder.SetCarLighting(!layout && (std::filesystem::exists(source.parent_path() / "data.acd", carEc) ||
                                       std::filesystem::exists(source.parent_path() / "data" / "car.ini", carEc)));
    // Tracks have no liveries; a car's skin replaces textures in every pass that reads them.
    const std::optional<std::filesystem::path> skin =
        layout ? std::nullopt : ResolveSkinDirectory(sources.front().file, options.skin);

    // Three passes, one model at a time, each reading only what it needs: a track's kn5 run to
    // hundreds of megabytes. First which textures the whole import samples, then those textures
    // from whichever model carries each, then materials and geometry.
    const float modelCount = static_cast<float>(sources.size());
    float modelIndex = 0.0f;
    for (const Kn5LayoutModel& placed : sources)
    {
        Kn5Model tables = Kn5Reader::Load(placed.file, Kn5ReadScope::Tables);
        FoldTextureCase(tables);
        builder.CollectTextureUse(tables);
        reportProgress(kTablesShare * (++modelIndex / modelCount));
    }
    modelIndex = 0.0f;
    for (const Kn5LayoutModel& placed : sources)
    {
        Kn5Model textures = Kn5Reader::Load(placed.file, Kn5ReadScope::Textures);
        FoldTextureCase(textures);
        if (skin.has_value())
        {
            ApplySkin(textures, *skin);
        }
        builder.WriteTextures(
            textures,
            [&](float fraction)
            {
                reportProgress(kTablesShare + kTexturesShare * ((modelIndex + fraction) / modelCount));
            });
        ++modelIndex;
    }

    std::vector<size_t> roots;
    modelIndex = 0.0f;
    for (const Kn5LayoutModel& placed : sources)
    {
        reportProgress(kTablesShare + kTexturesShare + kGeometryShare * (modelIndex++ / modelCount));
        Kn5Model model = Kn5Reader::Load(placed.file);
        report.foldedTextureNames += FoldTextureCase(model);
        ++report.models;
        if (skin.has_value())
        {
            report.skin = skin->filename().string();
            report.skinTextures = ApplySkin(model, *skin).size();
        }

        builder.AddMaterials(model);
        std::vector<std::string> names;
        if (!options.keepVariants)
        {
            CollectNodeNames(model.root, names);
        }
        builder.SetLowResTwins(LowResTwins(names));

        std::optional<size_t> root = builder.Emit(model.root);
        if (!root.has_value())
        {
            continue;
        }
        if (placed.position != std::array<float, 3>{} || placed.rotationDegrees != std::array<float, 3>{})
        {
            // Under the axis root, so the placement is written in AC's frame like everything else.
            root = builder.AddPlacement(placed.file.filename().string(), LayoutModelMatrix(placed.position, placed.rotationDegrees), *root);
        }
        roots.push_back(*root);
    }
    const size_t sceneRoot = builder.AddRoot(name, roots);
    reportProgress(kTablesShare + kTexturesShare + kGeometryShare);

    // A car's own figures, from the data next to its kn5. A car whose data cannot be read still
    // imports; it drives on the tuning's defaults.
    if (!layout)
    {
        std::string problem;
        if (std::optional<VehicleCarSpec> spec = AcCarData::ReadCarFolder(source.parent_path(), &problem))
        {
            // Its tyres go into the shared library, under the game's folder name for the car.
            TyreLibrary::AdoptCarTyres(*spec, source.parent_path().filename().string());
            builder.SetVehicle(*spec);
            report.carData = DescribeCarSpec(*spec);
        }
        else if (!problem.empty())
        {
            report.carDataProblem = problem;
            LOG_WARN("kn5 '{}': the car's data was not imported: {}", source.filename().string(), problem);
        }

        // Its sounds: the FMOD bank under sfx/, as WAVs and a .sounds.yaml beside the glTF.
        if (const std::filesystem::path bank = FindAcCarSoundBank(source); !bank.empty())
        {
            std::string soundError;
            if (ImportFmodBank(bank, FindAcSoundGuids(source), SoundBankPathForModel(gltfPath), soundError))
            {
                report.carSounds = true;
                LOG_INFO("kn5 '{}': sounds imported from '{}'", source.filename().string(), bank.filename().string());
            }
            else
            {
                report.carSoundsProblem = soundError;
                LOG_WARN("kn5 '{}': the car's sounds were not imported: {}", source.filename().string(), soundError);
            }
        }
    }

    const std::string binaryUri = "buffers/" + name + ".bin";
    const Json document = builder.BuildDocument(sceneRoot, binaryUri);
    WriteFileBytes(binaryPath, builder.Binary().data(), builder.Binary().size());
    const std::string text = document.dump();
    WriteFileBytes(gltfPath, text.data(), text.size());
    reportProgress(1.0f);

    report.gltfPath = gltfPath;
    report.nodes = builder.NodeCount();
    report.images = builder.ImageCount();

    const std::string sourceName = source.filename().string();
    LOG_INFO(
        "kn5 '{}': {} model(s), {} nodes ({} transforms, {} meshes, {} empty, {} variants dropped, {} never rendered), {} triangles, "
        "{} images, {} materials",
        sourceName,
        report.models,
        report.nodes,
        report.transforms,
        report.meshes,
        report.emptyMeshes,
        report.droppedVariants,
        report.hiddenMeshes,
        report.triangles,
        report.images,
        report.materials);
    if (report.collisionMeshes > 0)
    {
        LOG_INFO(
            "kn5 '{}': {} physics meshes ({} triangles) imported as collision only",
            sourceName,
            report.collisionMeshes,
            report.collisionTriangles);
    }
    if (report.shadowlessMeshes > 0 || report.distanceLimitedMeshes > 0)
    {
        LOG_INFO(
            "kn5 '{}': {} meshes cast no shadow, {} are drawn only within a camera distance range (LODs)",
            sourceName,
            report.shadowlessMeshes,
            report.distanceLimitedMeshes);
    }
    if (!report.skin.empty())
    {
        LOG_INFO("kn5 '{}': skin '{}' ({} textures)", sourceName, report.skin, report.skinTextures);
    }
    if (!report.carData.empty())
    {
        LOG_INFO("kn5 '{}': the car's own data: {}", sourceName, report.carData);
    }
    if (report.foldedTextureNames > 0)
    {
        LOG_INFO("kn5 '{}': folded {} texture names that differed only in case", sourceName, report.foldedTextureNames);
    }
    if (report.scrubbedAttributes > 0 || report.scrubbedMatrices > 0)
    {
        LOG_WARN(
            "kn5 '{}': replaced {} non-finite vertex attributes and dropped {} non-finite node matrices",
            sourceName,
            report.scrubbedAttributes,
            report.scrubbedMatrices);
    }
    return report;
}
}
}
