#include <engine/logic/editor_world.h>
#include <engine/scene/sun_position.h>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Every value is exact in binary floating point, so the YAML round trip can be compared with ==.
SceneEnvironment MakeEnvironment()
{
    SceneEnvironment environment{};
    environment.mode = EnvironmentMode::Hdri;
    environment.atmosphere.groundAlbedo = glm::vec3(0.25f, 0.5f, 0.125f);
    environment.atmosphere.groundPlane = true;
    environment.atmosphere.seamlessHorizon = true;
    environment.atmosphere.rayleighDensityScale = 2.0f;
    environment.atmosphere.mieDensityScale = 0.5f;
    environment.atmosphere.mieAnisotropy = 0.75f;
    environment.atmosphere.ozoneDensityScale = 0.0f;
    environment.atmosphere.aerialPerspectiveDistanceScale = 100.0f;
    environment.atmosphere.sunAngularDiameterDegrees = 1.5f;
    environment.hdri.path = "C:/hdri/sky.exr";
    environment.hdri.uuid = "33333333-3333-4333-8333-333333333333";
    environment.hdri.intensity = 1500.0f;
    environment.hdri.rotationDegrees = -90.0f;
    environment.heightFog.enabled = true;
    environment.heightFog.density = 0.0078125f;
    environment.heightFog.heightFalloff = 0.0625f;
    environment.heightFog.fogHeight = -12.5f;
    environment.heightFog.startDistance = 40.0f;
    environment.heightFog.maxOpacity = 0.75f;
    environment.heightFog.albedo = glm::vec3(0.5f, 0.75f, 1.0f);
    environment.heightFog.anisotropy = 0.25f;
    environment.clouds.enabled = true;
    environment.clouds.coverage = 0.625f;
    environment.clouds.baseAltitude = 1250.0f;
    environment.clouds.thickness = 1750.0f;
    environment.clouds.density = 0.03125f;
    environment.clouds.shapeScale = 5000.0f;
    environment.clouds.detailScale = 750.0f;
    environment.clouds.weatherScale = 30000.0f;
    environment.clouds.billows = 0.5f;
    environment.clouds.forwardAnisotropy = 0.75f;
    environment.clouds.backAnisotropy = -0.25f;
    environment.clouds.backWeight = 0.125f;
    environment.clouds.albedo = 0.875f;
    environment.clouds.ambientScale = 1.5f;
    environment.clouds.hazeDistance = 20000.0f;
    environment.clouds.diffusion = 0.375f;
    environment.clouds.ambientOcclusion = 0.625f;
    environment.timeOfDay.enabled = true;
    environment.timeOfDay.hours = 7.5f;
    environment.timeOfDay.dayOfYear = 100;
    environment.timeOfDay.latitudeDegrees = 42.5f;
    environment.timeOfDay.northDegrees = -30.0f;
    environment.timeOfDay.timeScale = 60.0f;
    environment.timeOfDay.moonEnabled = false;
    environment.timeOfDay.moonPhase = 0.25f;
    environment.timeOfDay.moonBrightness = 2.0f;
    environment.timeOfDay.nightSkyLuminance = 0.03125f;
    return environment;
}

void RoundTripsThroughYaml()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    const SceneEnvironment environment = MakeEnvironment();
    world->SetEnvironment(environment);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_test.yaml";
    SaveEditorSceneDataToFile(world->CaptureSceneData(), path.string());
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment == environment, "the environment did not survive the YAML round trip");

    std::unique_ptr<IEditorWorld> other = CreateEditorWorld();
    other->ApplySceneData(loaded);
    Require(other->GetEnvironment() == environment, "ApplySceneData did not restore the environment");
}

// A scene saved before environments existed: everything else as today, no environment node.
void MissingNodeLoadsAsNone()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->SetEnvironment(MakeEnvironment());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_legacy.yaml";
    SaveEditorSceneDataToFile(world->CaptureSceneData(), path.string());
    std::string yaml;
    {
        std::ifstream in(path);
        yaml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t begin = yaml.find("\nenvironment:");
    const size_t end = yaml.find("\neditor:");
    Require(begin != std::string::npos && end != std::string::npos && begin < end,
            "the saved scene has an environment node before the editor node");
    yaml.erase(begin, end - begin);
    {
        std::ofstream out(path, std::ios::trunc);
        out << yaml;
    }
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment.mode == EnvironmentMode::None, "a scene without an environment node must load as None");
}

// A scene saved before the fog existed: an environment node without height_fog reads as fog off.
void MissingFogNodeLoadsAsOff()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->SetEnvironment(MakeEnvironment());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_nofog.yaml";
    SaveEditorSceneDataToFile(world->CaptureSceneData(), path.string());
    std::string yaml;
    {
        std::ifstream in(path);
        yaml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t begin = yaml.find("\n  height_fog:");
    const size_t end = yaml.find("\neditor:");
    Require(begin != std::string::npos && end != std::string::npos && begin < end,
            "the saved scene has a height_fog node at the end of the environment node");
    yaml.erase(begin, end - begin);
    {
        std::ofstream out(path, std::ios::trunc);
        out << yaml;
    }
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment.mode == EnvironmentMode::Hdri, "the rest of the environment still loads");
    Require(loaded.environment.heightFog == HeightFogSettings{}, "a scene without height_fog must load with the fog off");
    Require(!loaded.environment.heightFog.enabled, "and the default is off");
}

// A scene saved before the clouds existed: an environment node without clouds reads as clouds off.
void MissingCloudsNodeLoadsAsOff()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->SetEnvironment(MakeEnvironment());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_noclouds.yaml";
    SaveEditorSceneDataToFile(world->CaptureSceneData(), path.string());
    std::string yaml;
    {
        std::ifstream in(path);
        yaml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t begin = yaml.find("\n  clouds:");
    const size_t end = yaml.find("\neditor:");
    Require(begin != std::string::npos && end != std::string::npos && begin < end,
            "the saved scene has a clouds node at the end of the environment node");
    yaml.erase(begin, end - begin);
    {
        std::ofstream out(path, std::ios::trunc);
        out << yaml;
    }
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment.heightFog == MakeEnvironment().heightFog, "the fog before it still loads");
    Require(loaded.environment.clouds == CloudSettings{}, "a scene without clouds must load with the clouds off");
    Require(!loaded.environment.clouds.enabled, "and the default is off");
}

// A scene saved before the seamless horizon existed: the atmosphere node has no such key, and reads as off.
void MissingSeamlessHorizonLoadsAsOff()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->SetEnvironment(MakeEnvironment());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_noseam.yaml";
    SaveEditorSceneDataToFile(world->CaptureSceneData(), path.string());
    std::string yaml;
    {
        std::ifstream in(path);
        yaml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t begin = yaml.find("\n    seamless_horizon:");
    const size_t end = begin == std::string::npos ? begin : yaml.find('\n', begin + 1);
    Require(begin != std::string::npos && end != std::string::npos, "the saved scene has a seamless_horizon key");
    yaml.erase(begin, end - begin);
    {
        std::ofstream out(path, std::ios::trunc);
        out << yaml;
    }
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment.atmosphere.groundPlane, "the atmosphere settings before it still load");
    Require(!loaded.environment.atmosphere.seamlessHorizon, "a scene without seamless_horizon must load with the ground in the sky");
}

void StartupSceneHasAtmosphereAndSun()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->CreateTwoCubeTestScene();
    Require(world->GetEnvironment().mode == EnvironmentMode::Atmosphere, "the startup scene uses the atmosphere");
    int directionalLights = 0;
    float sunIntensity = 0.0f;
    world->ForEachLight(
        [&](entt::entity, const TagComponent&, const TransformComponent&, const LightComponent& light)
        {
            if (light.type == LightType::Directional)
            {
                ++directionalLights;
                sunIntensity = light.intensity;
            }
        });
    Require(directionalLights == 1, "the startup scene has one sun");
    Require(sunIntensity == kDefaultSunIlluminanceLux, "the sun has the default illuminance");
}

// A scene saved before the time of day existed: no time_of_day node reads as off, so its sun keeps
// its hand-set rotation.
void MissingTimeOfDayNodeLoadsAsOff()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->SetEnvironment(MakeEnvironment());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_notime.yaml";
    SaveEditorSceneDataToFile(world->CaptureSceneData(), path.string());
    std::string yaml;
    {
        std::ifstream in(path);
        yaml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t begin = yaml.find("\n  time_of_day:");
    const size_t end = yaml.find("\neditor:");
    Require(begin != std::string::npos && end != std::string::npos && begin < end,
            "the saved scene has a time_of_day node at the end of the environment node");
    yaml.erase(begin, end - begin);
    {
        std::ofstream out(path, std::ios::trunc);
        out << yaml;
    }
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment.clouds == MakeEnvironment().clouds, "the clouds before it still load");
    Require(loaded.environment.timeOfDay == TimeOfDaySettings{}, "a scene without time_of_day must load with the defaults");
    Require(!loaded.environment.timeOfDay.enabled, "and the default is off");
}

bool Near(float a, float b, float tolerance)
{
    return std::abs(a - b) <= tolerance;
}

bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance)
{
    return glm::length(a - b) <= tolerance;
}

TimeOfDaySettings MakeTime(float hours, int day, float latitude)
{
    TimeOfDaySettings time{};
    time.enabled = true;
    time.hours = hours;
    time.dayOfYear = day;
    time.latitudeDegrees = latitude;
    return time;
}

// The northern-hemisphere arc: up in the east, highest due south at solar noon, down in the west.
void SunFollowsNorthernArc()
{
    Require(Near(SolarDeclinationDegrees(172), 23.44f, 0.05f), "the June solstice puts the sun 23.44 degrees north");
    Require(Near(SolarDeclinationDegrees(355), -23.44f, 0.05f), "the December solstice puts it 23.44 degrees south");
    Require(std::abs(SolarDeclinationDegrees(80)) < 1.0f, "the March equinox has it on the equator");

    for (const int day : {1, 80, 172, 279, 355})
    {
        for (const float latitude : {30.0f, 35.7f, 51.5f, 66.0f})
        {
            const float declination = SolarDeclinationDegrees(day);
            const SolarAngles noon = ComputeSolarAngles(MakeTime(12.0f, day, latitude));
            Require(Near(noon.elevationDegrees, 90.0f - latitude + declination, 0.01f), "solar noon stands at 90 - latitude + declination");
            Require(Near(noon.azimuthDegrees, 180.0f, 0.01f), "solar noon is due south north of the tropic");

            const SolarAngles morning = ComputeSolarAngles(MakeTime(9.0f, day, latitude));
            const SolarAngles evening = ComputeSolarAngles(MakeTime(15.0f, day, latitude));
            Require(morning.azimuthDegrees > 0.0f && morning.azimuthDegrees < 180.0f, "the morning sun is in the east");
            Require(evening.azimuthDegrees > 180.0f && evening.azimuthDegrees < 360.0f, "the afternoon sun is in the west");
            Require(Near(morning.elevationDegrees, evening.elevationDegrees, 0.01f), "the arc is symmetric about noon");
            Require(Near(morning.azimuthDegrees, 360.0f - evening.azimuthDegrees, 0.01f), "mirrored about the meridian");
            Require(morning.elevationDegrees < noon.elevationDegrees, "and lower than at noon");

            const SolarAngles midnight = ComputeSolarAngles(MakeTime(0.0f, day, latitude));
            Require(Near(midnight.elevationDegrees, declination - (90.0f - latitude), 0.01f), "midnight is the lower culmination");
        }
    }

    // At the equinox the sun rises (near enough) due east at 06:00.
    const SolarAngles sunrise = ComputeSolarAngles(MakeTime(6.0f, 80, 40.0f));
    Require(Near(sunrise.elevationDegrees, 0.0f, 1.0f), "at 06:00 on the equinox the sun is on the horizon");
    Require(Near(sunrise.azimuthDegrees, 90.0f, 1.0f), "in the east");

    Require(ComputeSolarAngles(MakeTime(0.0f, 172, 70.0f)).elevationDegrees > 0.0f, "the June midnight sun above the Arctic circle");
}

// Out-of-range settings: southern latitudes clamp to the equator, hours wrap.
void TimeOfDayClamps()
{
    const SolarAngles equator = ComputeSolarAngles(MakeTime(10.0f, 200, 0.0f));
    const SolarAngles south = ComputeSolarAngles(MakeTime(10.0f, 200, -40.0f));
    Require(equator.elevationDegrees == south.elevationDegrees && equator.azimuthDegrees == south.azimuthDegrees,
            "a southern latitude clamps to the equator");
    Require(ClampTimeOfDaySettings(MakeTime(10.0f, 200, 120.0f)).latitudeDegrees == 90.0f, "and past the pole to the pole");
    Require(Near(ClampTimeOfDaySettings(MakeTime(25.5f, 1, 0.0f)).hours, 1.5f, 1e-5f), "25.5 h wraps to 1.5 h");
    Require(Near(ClampTimeOfDaySettings(MakeTime(-1.0f, 1, 0.0f)).hours, 23.0f, 1e-5f), "-1 h wraps to 23 h");
    Require(ClampTimeOfDaySettings(MakeTime(24.0f, 1, 0.0f)).hours == 0.0f, "24 h is midnight");
    Require(ClampTimeOfDaySettings(MakeTime(12.0f, 400, 0.0f)).dayOfYear == 365, "the day clamps into the year");
}

// The light's rotation as the renderer builds it (BuildLightRotation): XYZ Euler, shining along -Y.
glm::vec3 ShineDirection(const glm::vec3& rotationDegrees)
{
    glm::mat4 rotation(1.0f);
    rotation = glm::rotate(rotation, glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    rotation = glm::rotate(rotation, glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    rotation = glm::rotate(rotation, glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)));
}

void SunDirectionInWorld()
{
    TimeOfDaySettings time = MakeTime(12.0f, 172, 35.7f);
    const glm::vec3 noon = ComputeDirectionToSun(time);
    Require(noon.z > 0.0f && Near(noon.x, 0.0f, 1e-5f), "with north along -Z the noon sun is toward +Z, the south");
    Require(Near(noon.y, std::sin(glm::radians(ComputeSolarAngles(time).elevationDegrees)), 1e-5f), "at its elevation");
    time.hours = 9.0f;
    Require(ComputeDirectionToSun(time).x > 0.0f, "the morning sun is toward +X, the east");
    time.hours = 12.0f;
    time.northDegrees = 90.0f;
    // North turned 90 degrees about +Y: -Z goes to -X, so the south is +X.
    Require(Near(ComputeDirectionToSun(time), glm::vec3(noon.z, noon.y, 0.0f), 1e-5f), "turning north turns the sun with it");

    // Every direction the sun takes round-trips through the light's rotation.
    for (float hours = 0.0f; hours < 24.0f; hours += 0.75f)
    {
        for (const float north : {0.0f, 37.0f, -120.0f})
        {
            TimeOfDaySettings sample = MakeTime(hours, 100, 51.5f);
            sample.northDegrees = north;
            const glm::vec3 toSun = ComputeDirectionToSun(sample);
            Require(Near(ShineDirection(DirectionalLightRotationDegrees(toSun)), -toSun, 1e-4f), "the light shines away from the sun");
        }
    }
    Require(Near(ShineDirection(DirectionalLightRotationDegrees(glm::vec3(0.0f, 1.0f, 0.0f))), glm::vec3(0.0f, -1.0f, 0.0f), 1e-5f),
            "an overhead sun shines straight down");
}

// The moon on the ecliptic, a phase of a turn east of the sun.
void MoonFollowsPhase()
{
    for (const int day : {1, 80, 172, 279})
    {
        for (const float hours : {0.0f, 6.5f, 13.0f, 21.25f})
        {
            TimeOfDaySettings time = MakeTime(hours, day, 35.7f);
            time.northDegrees = 25.0f;
            time.moonPhase = 0.5f;
            Require(Near(ComputeDirectionToMoon(time), -ComputeDirectionToSun(time), 1e-4f), "the full moon stands opposite the sun");
            time.moonPhase = 0.0f;
            Require(Near(ComputeDirectionToMoon(time), ComputeDirectionToSun(time), 1e-4f), "the new moon stands with the sun");
        }
    }

    // A first quarter is 90 degrees east of the sun: at the equinox it is due south at sunset.
    TimeOfDaySettings quarter = MakeTime(18.0f, 80, 35.7f);
    quarter.moonPhase = 0.25f;
    const SolarAngles moon = ComputeLunarAngles(quarter);
    Require(Near(moon.azimuthDegrees, 180.0f, 0.5f), "the first quarter is south at sunset");
    // At the equinox the sun is at ecliptic longitude 0, so the quarter is at 90: the June
    // solstice point, declination +23.44, culminating at 90 - latitude + 23.44.
    Require(Near(moon.elevationDegrees, 90.0f - 35.7f + 23.44f, 0.5f), "as high as the June sun at noon");

    Require(Near(MoonPhaseIlluminanceFraction(0.5f), 1.0f, 1e-6f), "the full moon is the full moon");
    const float firstQuarter = MoonPhaseIlluminanceFraction(0.25f);
    Require(firstQuarter > 0.07f && firstQuarter < 0.11f, "a quarter moon gives about a tenth of the light");
    Require(Near(firstQuarter, MoonPhaseIlluminanceFraction(0.75f), 1e-6f), "both quarters alike");
    Require(MoonPhaseIlluminanceFraction(0.0f) < 1e-3f, "the new moon gives next to none");
}

void MoonlightTakesOverAtNight()
{
    TimeOfDaySettings time = MakeTime(12.0f, 279, 35.7f);
    Require(!ComputeMoonlight(time).has_value(), "by day the sun lights the scene");
    time.hours = 0.0f;
    const std::optional<SkyLight> night = ComputeMoonlight(time);
    Require(night.has_value(), "at midnight the moon does");
    Require(Near(night->directionToLight, ComputeDirectionToMoon(time), 1e-6f), "from where it stands");
    Require(Near(night->illuminance, kMoonlightColor * kFullMoonIlluminanceLux, 1e-6f), "with the full moon's light");
    Require(night->directionToLight.y > 0.0f, "the full moon is up at midnight");
    time.moonBrightness = 3.0f;
    Require(Near(ComputeMoonlight(time)->illuminance, 3.0f * night->illuminance, 1e-6f), "brightness scales it");

    time.moonEnabled = false;
    Require(!ComputeMoonlight(time).has_value(), "with the moon off the sun stays");
    time.moonEnabled = true;
    time.enabled = false;
    Require(!ComputeMoonlight(time).has_value(), "and with the clock off");

    // Through dusk the sun keeps the sky until it is past the threshold.
    time.enabled = true;
    for (float hours = 16.0f; hours < 22.0f; hours += 0.05f)
    {
        time.hours = hours;
        const bool moonlit = ComputeMoonlight(time).has_value();
        Require(moonlit == (ComputeSolarAngles(time).elevationDegrees < kMoonTakesOverSunElevationDegrees), "the swap is at the threshold");
    }
}

glm::vec3 SunShineDirection(const IEditorWorld& world)
{
    glm::vec3 direction(0.0f);
    world.ForEachLight(
        [&](entt::entity, const TagComponent&, const TransformComponent& transform, const LightComponent& light)
        {
            if (light.type == LightType::Directional)
            {
                direction = ShineDirection(transform.rotationDegrees);
            }
        });
    return direction;
}

// The startup scene's sun follows the clock, and a change of time turns it.
void SceneSunFollowsClock()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->CreateTwoCubeTestScene();
    SceneEnvironment environment = world->GetEnvironment();
    Require(environment.timeOfDay.enabled, "a new scene's sun follows the clock");
    Require(Near(SunShineDirection(*world), -ComputeDirectionToSun(environment.timeOfDay), 1e-4f), "the sun stands where the clock puts it");
    const SolarAngles startup = ComputeSolarAngles(environment.timeOfDay);
    Require(startup.elevationDegrees > 30.0f && startup.elevationDegrees < 50.0f && startup.azimuthDegrees > 180.0f &&
                startup.azimuthDegrees < 240.0f,
            "the startup sun is an afternoon one in the south-west");

    environment.timeOfDay.hours = 8.0f;
    world->SetEnvironment(environment);
    Require(Near(SunShineDirection(*world), -ComputeDirectionToSun(environment.timeOfDay), 1e-4f), "setting the time turns the sun");

    // Off, the sun keeps whatever rotation it has.
    const glm::vec3 before = SunShineDirection(*world);
    environment.timeOfDay.enabled = false;
    environment.timeOfDay.hours = 18.0f;
    world->SetEnvironment(environment);
    Require(Near(SunShineDirection(*world), before, 1e-6f), "with the clock off the sun stays put");

    // Loading a scene places the sun by its clock.
    SerializedSceneData data = world->CaptureSceneData();
    data.environment.timeOfDay.enabled = true;
    data.environment.timeOfDay.hours = 16.5f;
    std::unique_ptr<IEditorWorld> other = CreateEditorWorld();
    other->ApplySceneData(data);
    Require(Near(SunShineDirection(*other), -ComputeDirectionToSun(data.environment.timeOfDay), 1e-4f), "a loaded scene's sun follows its clock");
}

int CountLights(const IEditorWorld& world, LightType type)
{
    int count = 0;
    world.ForEachLight(
        [&](entt::entity, const TagComponent&, const TransformComponent&, const LightComponent& light)
        {
            count += light.type == type ? 1 : 0;
        });
    return count;
}

void NewSceneKeepsOnlySunAndSky()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->CreateTwoCubeTestScene();
    world->SetSceneFilePath("scenes/test.yaml");
    world->GetGizmoSettings().operation = ImGuizmo::SCALE;
    SceneEnvironment hdri{};
    hdri.mode = EnvironmentMode::Hdri;
    world->SetEnvironment(hdri);

    world->CreateEmptyScene();
    Require(world->Registry().view<const ModelComponent>().size() == 0u, "a new scene has no models");
    Require(CountLights(*world, LightType::Directional) == 1, "a new scene has the startup sun");
    Require(world->GetEnvironment().mode == EnvironmentMode::Atmosphere, "a new scene uses the atmosphere");
    Require(world->GetEnvironment().heightFog.enabled, "a new scene has height fog");
    Require(world->GetEnvironment().clouds.enabled, "a new scene has clouds");
    Require(world->GetEnvironment().atmosphere.seamlessHorizon, "a new scene has a seamless horizon");
    Require(!world->HasSelection(), "nothing is selected in a new scene");
    Require(world->GetGizmoSettings().operation == ImGuizmo::SCALE, "a new scene keeps the gizmo settings");
}

void ClearKeepsEnvironmentAndFile()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->CreateTwoCubeTestScene();
    world->SetSceneFilePath("scenes/test.yaml");
    SceneEnvironment hdri{};
    hdri.mode = EnvironmentMode::Hdri;
    world->SetEnvironment(hdri);

    world->Clear();
    Require(world->GetSceneOrder().empty(), "clearing removes every entity");
    Require(!world->HasSelection(), "and the selection");
    Require(world->GetEnvironment() == hdri, "clearing keeps the environment");
    Require(world->GetSceneFilePath() == "scenes/test.yaml", "clearing keeps the scene file");
}
}

int main()
{
    try
    {
        RoundTripsThroughYaml();
        MissingNodeLoadsAsNone();
        MissingFogNodeLoadsAsOff();
        MissingCloudsNodeLoadsAsOff();
        MissingSeamlessHorizonLoadsAsOff();
        MissingTimeOfDayNodeLoadsAsOff();
        SunFollowsNorthernArc();
        TimeOfDayClamps();
        SunDirectionInWorld();
        MoonFollowsPhase();
        MoonlightTakesOverAtNight();
        SceneSunFollowsClock();
        StartupSceneHasAtmosphereAndSun();
        NewSceneKeepsOnlySunAndSky();
        ClearKeepsEnvironmentAndFile();
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene environment tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "scene environment tests passed\n";
    return 0;
}
