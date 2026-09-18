#include <engine/renderer/shadow_cascades.h>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>

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

ShadowCameraInput MakeCamera(const glm::vec3& position, const glm::vec3& forward)
{
    ShadowCameraInput camera{};
    camera.view = glm::lookAtRH(position, position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    camera.verticalFovRadians = glm::radians(60.0f);
    camera.aspect = 16.0f / 9.0f;
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.0f;
    return camera;
}

// A world point at the given view space position (view space looks down -Z).
glm::vec3 ViewToWorld(const ShadowCameraInput& camera, const glm::vec3& viewPoint)
{
    return glm::vec3(glm::inverse(camera.view) * glm::vec4(viewPoint, 1.0f));
}

glm::vec3 ToShadowCoordinates(const ShadowCascade& cascade, const glm::vec3& world)
{
    const glm::vec4 clip = cascade.viewProjection * glm::vec4(world, 1.0f);
    return glm::vec3(glm::vec2(clip) * 0.5f + 0.5f, clip.z);
}

void SplitsIncreaseAndStopAtTheShadowDistance()
{
    const ShadowCameraInput camera = MakeCamera(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    ShadowCascadeSettings settings{};
    settings.maxDistance = 60.0f;
    const ShadowCascades cascades = BuildShadowCascades(camera, glm::vec3(0.3f, -1.0f, 0.2f), settings);

    float previous = camera.nearPlane;
    for (const ShadowCascade& cascade : cascades)
    {
        Require(cascade.splitFar > previous, "cascade splits must increase");
        Require(cascade.texelWorldSize > 0.0f, "every cascade must have a texel size");
        previous = cascade.splitFar;
    }
    Require(std::abs(cascades.back().splitFar - 60.0f) < 1e-3f, "the last split must be the shadow distance");
    Require(cascades[0].texelWorldSize < cascades[3].texelWorldSize, "near cascades must be sharper than far ones");
}

void EverySliceFitsInsideItsCascade()
{
    const ShadowCameraInput camera = MakeCamera(glm::vec3(3.0f, 2.0f, 5.0f), glm::normalize(glm::vec3(0.4f, -0.3f, -1.0f)));
    const ShadowCascadeSettings settings{};
    const ShadowCascades cascades = BuildShadowCascades(camera, glm::vec3(-0.5f, -1.0f, 0.3f), settings);

    const float tanHalfY = std::tan(camera.verticalFovRadians * 0.5f);
    const float tanHalfX = tanHalfY * camera.aspect;
    float sliceNear = camera.nearPlane;
    for (const ShadowCascade& cascade : cascades)
    {
        for (float distance : {sliceNear, 0.5f * (sliceNear + cascade.splitFar), cascade.splitFar})
        {
            for (float sx : {-1.0f, 0.0f, 1.0f})
            {
                for (float sy : {-1.0f, 0.0f, 1.0f})
                {
                    const glm::vec3 world = ViewToWorld(
                        camera,
                        glm::vec3(sx * tanHalfX * distance, sy * tanHalfY * distance, -distance));
                    const glm::vec3 shadow = ToShadowCoordinates(cascade, world);
                    Require(shadow.x >= 0.0f && shadow.x <= 1.0f, "slice must fit the cascade horizontally");
                    Require(shadow.y >= 0.0f && shadow.y <= 1.0f, "slice must fit the cascade vertically");
                    Require(shadow.z > 0.0f && shadow.z < 1.0f, "slice must fit the cascade's depth range");
                }
            }
        }
        sliceNear = cascade.splitFar;
    }
}

void CastersBehindTheViewStillLandInTheDepthRange()
{
    const ShadowCameraInput camera = MakeCamera(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 lightDirection(0.0f, -1.0f, 0.0f);
    const ShadowCascades cascades = BuildShadowCascades(camera, lightDirection, ShadowCascadeSettings{});

    // Fifty metres above the first cascade, straight up the light: outside the camera frustum but
    // between the sun and what the camera sees.
    const glm::vec3 overhead = ViewToWorld(camera, glm::vec3(0.0f, 0.0f, -1.0f)) + glm::vec3(0.0f, 50.0f, 0.0f);
    const glm::vec3 shadow = ToShadowCoordinates(cascades[0], overhead);
    Require(shadow.z >= 0.0f && shadow.z < 1.0f, "a caster above the view must not be clipped by the near plane");
}

void CameraMotionMovesTheMapInWholeTexels()
{
    const glm::vec3 lightDirection(0.3f, -1.0f, 0.4f);
    const ShadowCascadeSettings settings{};
    const glm::vec3 fixedWorldPoint(1.0f, 0.0f, -4.0f);

    const ShadowCascades before = BuildShadowCascades(
        MakeCamera(glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)), lightDirection, settings);
    const ShadowCascades after = BuildShadowCascades(
        MakeCamera(glm::vec3(0.137f, 1.5f, -0.071f), glm::normalize(glm::vec3(0.05f, 0.0f, -1.0f))), lightDirection, settings);

    const float resolution = static_cast<float>(settings.resolution);
    for (uint32_t index = 0; index < kShadowCascadeCount; ++index)
    {
        Require(before[index].texelWorldSize == after[index].texelWorldSize, "turning the camera must not resize a cascade");
        const glm::vec2 texelBefore = glm::vec2(ToShadowCoordinates(before[index], fixedWorldPoint)) * resolution;
        const glm::vec2 texelAfter = glm::vec2(ToShadowCoordinates(after[index], fixedWorldPoint)) * resolution;
        const glm::vec2 shift = texelAfter - texelBefore;
        Require(std::abs(shift.x - std::round(shift.x)) < 0.02f, "a static point must move by whole texels in x");
        Require(std::abs(shift.y - std::round(shift.y)) < 0.02f, "a static point must move by whole texels in y");
    }
}

void CullingKeepsCastersInsideAndDropsOnesFarAway()
{
    const ShadowCameraInput camera = MakeCamera(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.2f, -1.0f, 0.1f));
    const ShadowCascades cascades = BuildShadowCascades(camera, lightDirection, ShadowCascadeSettings{});
    const ShadowCascade& first = cascades[0];

    const glm::vec3 inView = ViewToWorld(camera, glm::vec3(0.0f, 0.0f, -1.0f));
    Require(ShadowCascadeIntersectsSphere(first.viewProjection, inView, 0.1f), "a caster in the first slice must be drawn into it");

    // Thirty metres back up the light: outside the view, but between the sun and the slice.
    Require(
        ShadowCascadeIntersectsSphere(first.viewProjection, inView - lightDirection * 30.0f, 0.1f),
        "a caster up the light direction must be kept");
    Require(
        !ShadowCascadeIntersectsSphere(first.viewProjection, inView + lightDirection * 30.0f, 0.1f),
        "a caster beyond the slice, down the light, can shadow nothing in it and must be culled");

    const glm::vec3 sideways = inView + glm::vec3(500.0f, 0.0f, 0.0f);
    Require(!ShadowCascadeIntersectsSphere(first.viewProjection, sideways, 1.0f), "a caster far to the side must be culled");
    Require(ShadowCascadeIntersectsSphere(first.viewProjection, sideways, 600.0f), "a sphere large enough to reach the cascade must be kept");
}

void StraightDownLightIsHandled()
{
    const ShadowCameraInput camera = MakeCamera(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    const ShadowCascades cascades = BuildShadowCascades(camera, glm::vec3(0.0f, -1.0f, 0.0f), ShadowCascadeSettings{});
    const glm::vec3 shadow = ToShadowCoordinates(cascades[0], ViewToWorld(camera, glm::vec3(0.0f, 0.0f, -1.0f)));
    Require(std::isfinite(shadow.x) && std::isfinite(shadow.y) && std::isfinite(shadow.z), "a vertical light must not degenerate");
}
}

int main()
{
    try
    {
        SplitsIncreaseAndStopAtTheShadowDistance();
        EverySliceFitsInsideItsCascade();
        CastersBehindTheViewStillLandInTheDepthRange();
        CameraMotionMovesTheMapInWholeTexels();
        CullingKeepsCastersInsideAndDropsOnesFarAway();
        StraightDownLightIsHandled();
    }
    catch (const std::exception& error)
    {
        std::cerr << "shadow cascade tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "shadow cascade tests passed\n";
    return 0;
}
