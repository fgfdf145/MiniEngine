#include "shadow_cascades.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// Where cascade index ends, as a view distance, before the far plane cap.
float SplitDistance(uint32_t index, float nearPlane, float farPlane, float lambda)
{
    const float fraction = static_cast<float>(index) / static_cast<float>(kShadowCascadeCount);
    const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, fraction);
    const float uniform = nearPlane + (farPlane - nearPlane) * fraction;
    return lambda * logarithmic + (1.0f - lambda) * uniform;
}

// A rotation from world space into a light space looking down the light direction. It does not
// depend on the camera, which is what makes snapping to texels in this space stable.
glm::mat4 BuildLightRotation(const glm::vec3& lightDirection)
{
    const glm::vec3 forward = glm::normalize(lightDirection);
    const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::lookAtRH(glm::vec3(0.0f), forward, up);
}
}

ShadowCascades BuildShadowCascades(
    const ShadowCameraInput& camera,
    const glm::vec3& lightDirection,
    const ShadowCascadeSettings& settings)
{
    const float nearPlane = std::max(camera.nearPlane, 0.001f);
    const float farPlane = std::max(std::min(camera.farPlane, settings.maxDistance), nearPlane * 2.0f);
    const float tanHalfY = std::tan(camera.verticalFovRadians * 0.5f);
    const float tanHalfX = tanHalfY * camera.aspect;
    const glm::mat4 inverseView = glm::inverse(camera.view);
    const glm::mat4 lightRotation = BuildLightRotation(lightDirection);
    const float resolution = static_cast<float>(std::max(settings.resolution, 1u));

    ShadowCascades cascades{};
    float sliceNear = nearPlane;
    for (uint32_t index = 0; index < kShadowCascadeCount; ++index)
    {
        const float sliceFar = SplitDistance(index + 1, nearPlane, farPlane, settings.splitLambda);

        // The slice's eight corners in world space. View space looks down -Z.
        std::array<glm::vec3, 8> corners{};
        uint32_t cornerIndex = 0;
        for (float distance : {sliceNear, sliceFar})
        {
            for (float sx : {-1.0f, 1.0f})
            {
                for (float sy : {-1.0f, 1.0f})
                {
                    const glm::vec4 viewCorner(sx * tanHalfX * distance, sy * tanHalfY * distance, -distance, 1.0f);
                    corners[cornerIndex++] = glm::vec3(inverseView * viewCorner);
                }
            }
        }

        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : corners)
        {
            center += corner;
        }
        center /= static_cast<float>(corners.size());

        float radius = 0.0f;
        for (const glm::vec3& corner : corners)
        {
            radius = std::max(radius, glm::length(corner - center));
        }
        // The slice has the same shape wherever the camera points, so the radius is constant up to
        // rounding; quantizing it removes that rounding too, or the texel size would still wobble.
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const float texelWorldSize = 2.0f * radius / resolution;
        glm::vec3 lightCenter = glm::vec3(lightRotation * glm::vec4(center, 1.0f));
        lightCenter.x = std::floor(lightCenter.x / texelWorldSize) * texelWorldSize;
        lightCenter.y = std::floor(lightCenter.y / texelWorldSize) * texelWorldSize;

        // Light space looks down -Z, so a point's depth along the light is -z.
        const float centerDepth = -lightCenter.z;
        const glm::mat4 projection = glm::orthoRH_ZO(
            lightCenter.x - radius,
            lightCenter.x + radius,
            lightCenter.y - radius,
            lightCenter.y + radius,
            centerDepth - radius - settings.casterPullback,
            centerDepth + radius);

        cascades[index].viewProjection = projection * lightRotation;
        cascades[index].splitFar = sliceFar;
        cascades[index].texelWorldSize = texelWorldSize;
        sliceNear = sliceFar;
    }
    return cascades;
}

bool ShadowCascadeIntersectsSphere(const glm::mat4& viewProjection, const glm::vec3& center, float radius)
{
    const glm::vec4 clip = viewProjection * glm::vec4(center, 1.0f);
    // The projection is orthographic and the view a rotation, so each clip axis is a scaled world
    // axis: the length of its row is how many clip units one metre covers along it.
    const auto rowScale = [&](int row)
    {
        return glm::length(glm::vec3(viewProjection[0][row], viewProjection[1][row], viewProjection[2][row]));
    };
    const float rx = radius * rowScale(0);
    const float ry = radius * rowScale(1);
    const float rz = radius * rowScale(2);
    return std::abs(clip.x) <= 1.0f + rx &&
           std::abs(clip.y) <= 1.0f + ry &&
           clip.z >= -rz &&
           clip.z <= 1.0f + rz;
}
}
