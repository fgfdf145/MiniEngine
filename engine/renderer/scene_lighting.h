#pragma once

#include <engine/scene/scene_components.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// What the light selection needs to know about one scene light. Kept separate from the GPU light
// struct so the ranking can be tested without the Vulkan backend.
struct SceneLightCandidate
{
    LightType type = LightType::Point;
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    // Lumens for point, spot and area lights, lux for directional, cd/m^2 for ambient.
    float intensity = 0.0f;
};

// The fallback ambient luminance in cd/m^2, used only when the scene has no Ambient light. It
// stands for no physical light: it only keeps a scene without lights from rendering black. At the
// default EV100 it is the {0.05, 0.05, 0.08} the shader used before exposure existed.
glm::vec3 GetFallbackAmbientLuminance();

struct SceneLightSelection
{
    // Indices into the candidate span of the lights the shader evaluates, most important first.
    // Ambient lights never appear here: they are folded into ambientLuminance instead, so they do
    // not take up one of the limited shader light slots.
    std::vector<uint32_t> selected;
    // Linear RGB luminance in cd/m^2: the sum of every Ambient light, or the fallback when the
    // scene has none.
    glm::vec3 ambientLuminance{0.0f};
    bool usesFallbackAmbient = false;
    // Non-ambient lights left out because the scene has more than maxLights of them.
    uint32_t droppedCount = 0;
};

// Picks which lights the shader evaluates when there are more than it has room for, and sums the
// ambient lights.
//
// Directional lights rank first, brightest first: they light the whole scene and the first one is
// the shadow caster (see SelectShadowCasterLight). Local lights follow, ranked by their illuminance
// at the camera, intensity / distance^2, which keeps the lights that matter most for what is on
// screen. Ties keep scene order, so the selection does not flicker between equal lights.
SceneLightSelection SelectSceneLights(
    std::span<const SceneLightCandidate> candidates,
    const glm::vec3& cameraPosition,
    uint32_t maxLights);

// The index into selection.selected of the light that casts shadows, or -1 when none does. Only
// directional lights cast shadows so far, and SelectSceneLights puts the brightest one first.
int32_t SelectShadowCasterLight(
    std::span<const SceneLightCandidate> candidates,
    const SceneLightSelection& selection);
}
