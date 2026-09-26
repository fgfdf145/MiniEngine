#include "scene_lighting.h"

#include "exposure.h"

#include <algorithm>

namespace me
{

namespace
{
// Distances below a metre are clamped so a light the camera sits inside does not outrank a
// brighter one just because the ratio blows up.
constexpr float kMinimumRankingDistanceSquared = 1.0f;

float RankingScore(const SceneLightCandidate& light, const glm::vec3& cameraPosition)
{
    const glm::vec3 offset = light.position - cameraPosition;
    return light.intensity / std::max(glm::dot(offset, offset), kMinimumRankingDistanceSquared);
}
}

glm::vec3 GetFallbackAmbientLuminance()
{
    return glm::vec3(0.05f, 0.05f, 0.08f) / ExposureFromEv100(kDefaultExposureEv100);
}

SceneLightSelection SelectSceneLights(
    std::span<const SceneLightCandidate> candidates,
    const glm::vec3& cameraPosition,
    uint32_t maxLights)
{
    SceneLightSelection selection{};

    std::vector<uint32_t> directional;
    std::vector<uint32_t> local;
    bool hasAmbientLight = false;
    for (uint32_t index = 0; index < static_cast<uint32_t>(candidates.size()); ++index)
    {
        const SceneLightCandidate& light = candidates[index];
        switch (light.type)
        {
        case LightType::Ambient:
            hasAmbientLight = true;
            selection.ambientLuminance += light.color * light.intensity;
            break;
        case LightType::Directional:
            directional.push_back(index);
            break;
        default:
            local.push_back(index);
            break;
        }
    }

    if (!hasAmbientLight)
    {
        selection.ambientLuminance = GetFallbackAmbientLuminance();
        selection.usesFallbackAmbient = true;
    }

    std::stable_sort(
        directional.begin(),
        directional.end(),
        [&](uint32_t lhs, uint32_t rhs)
        {
            return candidates[lhs].intensity > candidates[rhs].intensity;
        });

    std::vector<float> scores(candidates.size(), 0.0f);
    for (uint32_t index : local)
    {
        scores[index] = RankingScore(candidates[index], cameraPosition);
    }
    std::stable_sort(
        local.begin(),
        local.end(),
        [&](uint32_t lhs, uint32_t rhs)
        {
            return scores[lhs] > scores[rhs];
        });

    selection.selected = std::move(directional);
    selection.selected.insert(selection.selected.end(), local.begin(), local.end());
    if (selection.selected.size() > maxLights)
    {
        selection.droppedCount = static_cast<uint32_t>(selection.selected.size()) - maxLights;
        selection.selected.resize(maxLights);
    }
    return selection;
}

PlacedModelLight PlaceModelLight(const glm::mat4& modelMatrix, const glm::vec3& position, const glm::vec3& direction)
{
    PlacedModelLight placed{};
    placed.position = glm::vec3(modelMatrix * glm::vec4(position, 1.0f));
    const glm::vec3 worldDirection = glm::vec3(modelMatrix * glm::vec4(direction, 0.0f));
    const float length = glm::length(worldDirection);
    placed.direction = length > 1e-6f ? worldDirection / length : glm::normalize(direction);
    return placed;
}

int32_t SelectShadowCasterLight(
    std::span<const SceneLightCandidate> candidates,
    const SceneLightSelection& selection)
{
    if (!selection.selected.empty() &&
        candidates[selection.selected.front()].type == LightType::Directional &&
        candidates[selection.selected.front()].intensity > 0.0f)
    {
        return 0;
    }
    return -1;
}
}
