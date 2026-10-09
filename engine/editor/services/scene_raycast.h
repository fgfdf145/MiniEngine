#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <optional>

namespace me
{
class ISceneWorld;
class RendererWorld;

// Where a ray first meets what the scene draws: the world position, how far along the ray (m), and the
// entity whose mesh it hit.
struct SceneRayHit
{
    glm::dvec3 position{0.0};
    double distance = 0.0;
    entt::entity entity = entt::null;
};

// The first surface the ray from `origin` along `direction` (need not be unit) meets within
// `maxDistance`, among the solid meshes the scene draws: decals, the tops of water, skinned meshes
// (characters), see-through Blend materials and far levels of detail are passed through, as is
// `exclude` (the car being driven). Empty when it meets none.
std::optional<SceneRayHit> RaycastScene(
    const RendererWorld& renderWorld, const ISceneWorld& scene, const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance,
    entt::entity exclude = entt::null);
}
