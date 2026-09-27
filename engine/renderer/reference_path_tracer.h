#pragma once

#include "ray_tracing_bvh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace me
{

// The ground truth the DDGI probes are compared against (docs/design/2026-09-27-ddgi-design.md, step
// 6): a CPU path tracer over the same ray scene and ray materials the probe rays trace. Surfaces are
// Lambertian with their ray material's average albedo; a Mask surface stops the share of rays its
// coverage says; a single-sided surface's back is black, as the probe rays see it. Directional lights
// reach every hit through a shadow ray; the sky is one uniform radiance.

// What a ray sees of one submesh (RayMaterial in shaders/vulkan/ray_tracing_common.glsl), indexed by
// RayInstance::data.z.
struct ReferenceMaterial
{
    glm::vec3 albedo{0.0f};
    // The share of rays the surface stops: 1 for opaque, less for foliage cards.
    float coverage = 1.0f;
    glm::vec3 emission{0.0f};
    bool doubleSided = false;
};

// A directional light: the unit direction toward it and the illuminance it puts on a surface facing
// it (colour times intensity, lux).
struct ReferenceLight
{
    glm::vec3 directionToLight{0.0f, 1.0f, 0.0f};
    glm::vec3 illuminance{0.0f};
};

struct ReferenceSettings
{
    uint32_t samples = 256;
    // Paths end here at the latest; Russian roulette ends most earlier, without bias.
    uint32_t maxBounces = 32;
    // What a ray that leaves the scene brings, in every direction.
    glm::vec3 skyRadiance{0.0f};
};

// The irradiance / pi arriving at P from the hemisphere around N (unit), not counting the lights'
// direct beams: the sky, emission and every bounce. That is what the DDGI probes answer for, in the
// unit they store it. seed decorrelates calls.
glm::vec3 ReferenceIndirectIrradiance(
    const RayScene& scene,
    std::span<const ReferenceMaterial> materials,
    std::span<const ReferenceLight> lights,
    const glm::vec3& P,
    const glm::vec3& N,
    const ReferenceSettings& settings,
    uint64_t seed);

// The first surface a ray from origin along direction meets, for choosing the points to compare:
// position, the normal on the side the ray came from, and whether it is a surface the comparison
// can use (a front face, or either face of a double-sided one).
struct ReferenceSurface
{
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    bool valid = false;
};
ReferenceSurface ReferencePrimaryHit(
    const RayScene& scene,
    std::span<const ReferenceMaterial> materials,
    const glm::vec3& origin,
    const glm::vec3& direction);
}
