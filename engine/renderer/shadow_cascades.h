#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace me
{

// Cascaded shadow maps for the one directional light that casts shadows. The shader and the
// uniform block are sized for exactly this many cascades.
inline constexpr uint32_t kShadowCascadeCount = 4;

struct ShadowCascadeSettings
{
    // Width and height of each cascade's layer in texels.
    uint32_t resolution = 2048;
    // How far from the camera shadows reach, in metres. The camera's far plane caps it.
    float maxDistance = 80.0f;
    // Blend between logarithmic (1) and uniform (0) split placement. Logarithmic keeps the texel
    // density per screen pixel roughly even; the uniform share keeps the first cascade from being
    // wasted on the few centimetres in front of the near plane.
    float splitLambda = 0.8f;
    // How far behind a cascade's bounding sphere, along the light, casters are still rendered.
    // Geometry outside the camera view still throws shadows into it, and there is no depth clamp to
    // flatten it onto the near plane, so the near plane is pulled back instead.
    float casterPullback = 200.0f;
};

// The camera the cascades cover. The view matrix is world to view, looking down -Z, and the
// projection is a symmetric perspective one.
struct ShadowCameraInput
{
    glm::mat4 view{1.0f};
    float verticalFovRadians = 0.785398f;
    float aspect = 1.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

struct ShadowCascade
{
    // World to light clip space, with depth in [0, 1] and x, y in [-1, 1]. A clip space point maps
    // to shadow map coordinates as uv = xy * 0.5 + 0.5; Vulkan's viewport puts NDC y = -1 on row 0,
    // and so does a texture's v = 0, so neither side flips.
    glm::mat4 viewProjection{1.0f};
    // The view space distance, along the camera's forward axis, where this cascade ends.
    float splitFar = 0.0f;
    // The world size of one texel, which the shader scales its normal offset bias by.
    float texelWorldSize = 0.0f;
};

using ShadowCascades = std::array<ShadowCascade, kShadowCascadeCount>;

// lightDirection is the direction the light travels, as GpuLightData stores it. It need not be
// normalized but must not be zero.
//
// Every cascade is fitted to the bounding sphere of its slice of the camera frustum, so its size
// does not change as the camera turns, and its origin is snapped to whole texels in light space, so
// moving the camera moves the shadow map in whole texel steps. Together those keep shadow edges
// from crawling while the camera moves.
ShadowCascades BuildShadowCascades(
    const ShadowCameraInput& camera,
    const glm::vec3& lightDirection,
    const ShadowCascadeSettings& settings);

// Whether a world space sphere reaches into a cascade's volume, the box its orthographic
// viewProjection maps onto x, y in [-1, 1] and depth in [0, 1]. Conservative: a sphere near a
// corner may pass without touching the box, but one that touches it never fails.
bool ShadowCascadeIntersectsSphere(const glm::mat4& viewProjection, const glm::vec3& center, float radius);
}
