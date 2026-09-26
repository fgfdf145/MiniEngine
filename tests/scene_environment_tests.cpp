#include <engine/logic/editor_world.h>

#include <filesystem>
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
    return environment;
}

void RoundTripsThroughYaml()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    const SceneEnvironment environment = MakeEnvironment();
    world->SetEnvironment(environment);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_test.yaml";
    world->SaveSceneToFile(path.string());
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
    world->SaveSceneToFile(path.string());
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
    Require(!world->HasEntities(), "clearing removes every entity");
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
