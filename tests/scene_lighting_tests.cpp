#include <engine/renderer/scene_lighting.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearlyEqual(const glm::vec3& lhs, const glm::vec3& rhs)
{
    const glm::vec3 difference = glm::abs(lhs - rhs);
    return difference.x <= 1e-4f && difference.y <= 1e-4f && difference.z <= 1e-4f;
}

SceneLightCandidate MakeLight(LightType type, glm::vec3 position, float intensity, glm::vec3 color = glm::vec3(1.0f))
{
    SceneLightCandidate light{};
    light.type = type;
    light.position = position;
    light.intensity = intensity;
    light.color = color;
    return light;
}

void NoLightsUsesTheFallbackAmbient()
{
    const SceneLightSelection selection = SelectSceneLights({}, glm::vec3(0.0f), 8);
    Require(selection.selected.empty(), "no lights must select nothing");
    Require(selection.usesFallbackAmbient, "a scene without ambient lights must use the fallback");
    Require(NearlyEqual(selection.ambientLuminance, GetFallbackAmbientLuminance()), "the fallback ambient must be uploaded");
}

void AmbientLightsReplaceTheFallbackAndTakeNoSlot()
{
    const std::vector<SceneLightCandidate> lights = {
        MakeLight(LightType::Ambient, glm::vec3(0.0f), 2.0f, glm::vec3(1.0f, 0.5f, 0.0f)),
        MakeLight(LightType::Point, glm::vec3(0.0f), 100.0f),
        MakeLight(LightType::Ambient, glm::vec3(0.0f), 1.0f, glm::vec3(0.0f, 0.0f, 1.0f))};
    const SceneLightSelection selection = SelectSceneLights(lights, glm::vec3(0.0f), 8);

    Require(!selection.usesFallbackAmbient, "an ambient light must turn the fallback off");
    Require(NearlyEqual(selection.ambientLuminance, glm::vec3(2.0f, 1.0f, 1.0f)), "ambient lights must be summed as color * intensity");
    Require(selection.selected.size() == 1 && selection.selected[0] == 1, "ambient lights must not take a shader light slot");
}

void ZeroIntensityAmbientMakesTheSceneDark()
{
    const std::vector<SceneLightCandidate> lights = {MakeLight(LightType::Ambient, glm::vec3(0.0f), 0.0f)};
    const SceneLightSelection selection = SelectSceneLights(lights, glm::vec3(0.0f), 8);
    Require(!selection.usesFallbackAmbient, "a zero intensity ambient light must still turn the fallback off");
    Require(NearlyEqual(selection.ambientLuminance, glm::vec3(0.0f)), "a zero intensity ambient light must give no ambient");
}

void DirectionalLightsRankFirstBrightestFirst()
{
    const std::vector<SceneLightCandidate> lights = {
        MakeLight(LightType::Point, glm::vec3(0.0f), 1.0e6f),
        MakeLight(LightType::Directional, glm::vec3(0.0f), 100.0f),
        MakeLight(LightType::Directional, glm::vec3(0.0f), 50000.0f)};
    const SceneLightSelection selection = SelectSceneLights(lights, glm::vec3(0.0f), 8);

    Require(selection.selected.size() == 3, "all three lights fit");
    Require(selection.selected[0] == 2 && selection.selected[1] == 1, "directional lights must come first, brightest first");
    Require(selection.selected[2] == 0, "local lights must follow the directional ones");
    Require(SelectShadowCasterLight(lights, selection) == 0, "the brightest directional light casts the shadows");
}

void LocalLightsRankByIlluminanceAtTheCamera()
{
    const std::vector<SceneLightCandidate> lights = {
        MakeLight(LightType::Point, glm::vec3(100.0f, 0.0f, 0.0f), 1000.0f), // 0.1
        MakeLight(LightType::Spot, glm::vec3(2.0f, 0.0f, 0.0f), 1000.0f),    // 250
        MakeLight(LightType::Area, glm::vec3(10.0f, 0.0f, 0.0f), 1000.0f)};  // 10
    const SceneLightSelection selection = SelectSceneLights(lights, glm::vec3(0.0f), 2);

    Require(selection.selected.size() == 2, "the selection must stop at the light limit");
    Require(selection.selected[0] == 1 && selection.selected[1] == 2, "the nearest light must rank first");
    Require(selection.droppedCount == 1, "the far light must be reported as dropped");
    Require(SelectShadowCasterLight(lights, selection) == -1, "only a directional light casts shadows");
}

void TiesKeepSceneOrder()
{
    std::vector<SceneLightCandidate> lights;
    for (int index = 0; index < 5; ++index)
    {
        lights.push_back(MakeLight(LightType::Point, glm::vec3(3.0f, 0.0f, 0.0f), 500.0f));
    }
    const SceneLightSelection selection = SelectSceneLights(lights, glm::vec3(0.0f), 3);
    Require(selection.selected == std::vector<uint32_t>({0, 1, 2}), "equal lights must keep scene order");
}

void DarkDirectionalLightCastsNoShadow()
{
    const std::vector<SceneLightCandidate> lights = {MakeLight(LightType::Directional, glm::vec3(0.0f), 0.0f)};
    const SceneLightSelection selection = SelectSceneLights(lights, glm::vec3(0.0f), 8);
    Require(SelectShadowCasterLight(lights, selection) == -1, "a zero intensity sun must not render a shadow map");
}
}

int main()
{
    try
    {
        NoLightsUsesTheFallbackAmbient();
        AmbientLightsReplaceTheFallbackAndTakeNoSlot();
        ZeroIntensityAmbientMakesTheSceneDark();
        DirectionalLightsRankFirstBrightestFirst();
        LocalLightsRankByIlluminanceAtTheCamera();
        TiesKeepSceneOrder();
        DarkDirectionalLightCastsNoShadow();
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene lighting tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "scene lighting tests passed\n";
    return 0;
}
