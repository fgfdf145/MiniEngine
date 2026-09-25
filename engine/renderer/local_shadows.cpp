#include "local_shadows.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace me
{

namespace
{
// How much wider than the region its lookups land in a tile's frustum is: the guard band as a
// factor on tan(fov / 2).
constexpr float kGuardWidening =
    (static_cast<float>(kLocalShadowTileSize) * 0.5f) /
    (static_cast<float>(kLocalShadowTileSize) * 0.5f - kLocalShadowGuardTexels);

// The far plane sits a hair beyond the range, so a point exactly at the range still has a depth
// below 1 and the whole lit volume compares against the map.
constexpr float kFarPlaneMargin = 1.01f;

float ShadowFarPlane(float range)
{
    return std::max(range * kFarPlaneMargin, kLocalShadowNearPlane * 2.0f);
}

glm::mat4 BuildPerspectiveTile(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, float tanHalfFov, float range)
{
    const glm::mat4 view = glm::lookAtRH(position, position + forward, up);
    const glm::mat4 projection = glm::perspectiveRH_ZO(2.0f * std::atan(tanHalfFov), 1.0f, kLocalShadowNearPlane, ShadowFarPlane(range));
    return projection * view;
}

bool IsShadowedAsCube(const LocalShadowLight& light)
{
    return light.type != LightType::Spot || light.outerAngleRadians > kLocalShadowMaxSpotHalfAngleRadians;
}

LocalShadowTile MakeTile(uint32_t tileIndex, const glm::mat4& viewProjection, float tanHalfFov)
{
    LocalShadowTile tile{};
    tile.viewProjection = viewProjection;
    tile.atlasOffsetTexels = glm::uvec2(
        (tileIndex % kLocalShadowTilesPerRow) * kLocalShadowTileSize,
        (tileIndex / kLocalShadowTilesPerRow) * kLocalShadowTileSize);
    const float atlasSize = static_cast<float>(kLocalShadowAtlasSize);
    const float tileUv = static_cast<float>(kLocalShadowTileSize) / atlasSize;
    tile.atlasRect = glm::vec4(
        static_cast<float>(tile.atlasOffsetTexels.x) / atlasSize,
        static_cast<float>(tile.atlasOffsetTexels.y) / atlasSize,
        tileUv,
        tileUv);
    tile.texelScale = 2.0f * tanHalfFov / static_cast<float>(kLocalShadowTileSize);
    return tile;
}
}

glm::mat4 BuildLocalShadowCubeFace(const glm::vec3& position, float range, uint32_t face)
{
    // SelectCubeFace's order. The up vectors only have to be consistent between the map and the
    // lookup, which share these matrices.
    static const std::array<glm::vec3, kLocalShadowCubeFaceCount> kForward = {
        glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    static const std::array<glm::vec3, kLocalShadowCubeFaceCount> kUp = {
        glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)};
    const uint32_t index = std::min(face, kLocalShadowCubeFaceCount - 1);
    return BuildPerspectiveTile(position, kForward[index], kUp[index], kGuardWidening, range);
}

bool FrustumIntersectsSphere(const glm::mat4& viewProjection, const glm::vec3& center, float radius)
{
    // Gribb and Hartmann: the planes are sums of the matrix's rows. glm is column-major, so row i
    // is (m[0][i], m[1][i], m[2][i], m[3][i]). Zero-to-one depth makes the near plane row 2 alone.
    const auto row = [&viewProjection](int i)
    {
        return glm::vec4(viewProjection[0][i], viewProjection[1][i], viewProjection[2][i], viewProjection[3][i]);
    };
    const glm::vec4 r0 = row(0);
    const glm::vec4 r1 = row(1);
    const glm::vec4 r2 = row(2);
    const glm::vec4 r3 = row(3);
    const std::array<glm::vec4, 6> planes = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    for (const glm::vec4& plane : planes)
    {
        const float length = glm::length(glm::vec3(plane));
        if (length <= 0.0f)
        {
            continue;
        }
        const float distance = (glm::dot(glm::vec3(plane), center) + plane.w) / length;
        if (distance < -radius)
        {
            return false;
        }
    }
    return true;
}

LocalShadowPlan PlanLocalShadows(std::span<const LocalShadowLight> lights, const glm::mat4& cameraViewProjection)
{
    LocalShadowPlan plan;
    plan.firstTile.assign(lights.size(), -1);
    for (size_t index = 0; index < lights.size(); ++index)
    {
        const LocalShadowLight& light = lights[index];
        const bool local = light.type == LightType::Point || light.type == LightType::Spot || light.type == LightType::Area;
        if (!local || !light.castShadows || light.range <= 0.0f ||
            !FrustumIntersectsSphere(cameraViewProjection, light.position, light.range))
        {
            continue;
        }

        const bool cube = IsShadowedAsCube(light);
        const uint32_t needed = cube ? kLocalShadowCubeFaceCount : 1u;
        const uint32_t first = static_cast<uint32_t>(plan.tiles.size());
        if (first + needed > kLocalShadowTileCount)
        {
            ++plan.droppedCount;
            continue;
        }

        plan.firstTile[index] = static_cast<int32_t>(first);
        if (cube)
        {
            for (uint32_t face = 0; face < kLocalShadowCubeFaceCount; ++face)
            {
                plan.tiles.push_back(MakeTile(first + face, BuildLocalShadowCubeFace(light.position, light.range, face), kGuardWidening));
            }
        }
        else
        {
            const glm::vec3 forward = glm::normalize(light.direction);
            const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const float tanHalfFov = std::tan(light.outerAngleRadians) * kGuardWidening;
            plan.tiles.push_back(MakeTile(first, BuildPerspectiveTile(light.position, forward, up, tanHalfFov, light.range), tanHalfFov));
        }
    }
    return plan;
}
}
