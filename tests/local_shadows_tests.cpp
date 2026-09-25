#include <engine/renderer/local_shadows.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// The face selection and atlas lookup the shader uses, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/local_shadow_common.glsl>
}

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

constexpr float kGuard = kLocalShadowGuardTexels / static_cast<float>(kLocalShadowTileSize);

glm::mat4 MakeCamera(const glm::vec3& position, const glm::vec3& forward)
{
    const glm::mat4 view = glm::lookAtRH(position, position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
    projection[1][1] *= -1.0f;
    return projection * view;
}

LocalShadowLight MakeLight(LightType type, const glm::vec3& position, float range = 10.0f)
{
    LocalShadowLight light{};
    light.type = type;
    light.position = position;
    light.range = range;
    return light;
}

glm::vec3 RandomUnitVector(std::mt19937& random)
{
    std::normal_distribution<float> normal;
    glm::vec3 v(0.0f);
    while (glm::dot(v, v) < 1e-6f)
    {
        v = glm::vec3(normal(random), normal(random), normal(random));
    }
    return glm::normalize(v);
}

// The tile-local coordinates, [0, 1] over the tile, of a world point.
glm::vec3 TileLocal(const LocalShadowTile& tile, const glm::vec3& world)
{
    const glm::vec4 clip = tile.viewProjection * glm::vec4(world, 1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec3(glm::vec2(ndc) * 0.5f + 0.5f, ndc.z);
}

void CubeFacesCoverEveryDirectionWithTheGuardToSpare()
{
    const glm::vec3 lightPosition(1.0f, 2.0f, -3.0f);
    const float range = 12.0f;
    const LocalShadowLight light = MakeLight(LightType::Point, lightPosition, range);
    const LocalShadowPlan plan = PlanLocalShadows(std::span(&light, 1), MakeCamera(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
    Require(plan.firstTile.size() == 1 && plan.firstTile[0] == 0, "a point light in view gets the first tiles");
    Require(plan.tiles.size() == kLocalShadowCubeFaceCount, "a point light takes six tiles");

    // The lookups land at most this far from the tile's centre: the guard band plus a hair of float.
    const float limit = 0.5f - kGuard + 1e-4f;
    std::mt19937 random(7);
    for (int sample = 0; sample < 20000; ++sample)
    {
        const glm::vec3 direction = RandomUnitVector(random);
        const float distance = 0.2f + (range - 0.2f) * static_cast<float>(sample % 97) / 96.0f;
        const glm::vec3 world = lightPosition + direction * distance;
        const int face = shader::SelectCubeFace(direction);
        Require(face >= 0 && face < 6, "a face index is 0 to 5");
        const glm::vec3 local = TileLocal(plan.tiles[static_cast<size_t>(face)], world);
        Require(std::abs(local.x - 0.5f) <= limit && std::abs(local.y - 0.5f) <= limit,
                "a direction lands inside its face with the guard band to spare");
        Require(local.z > 0.0f && local.z < 1.0f, "every point within range has a depth in (0, 1)");
    }
}

void CubeFacesFollowTheSelectionOrder()
{
    const std::array<glm::vec3, 6> axes = {
        glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
        glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
    for (uint32_t face = 0; face < 6; ++face)
    {
        Require(shader::SelectCubeFace(axes[face]) == static_cast<int>(face), "the face order is +X, -X, +Y, -Y, +Z, -Z");
        const glm::mat4 viewProjection = BuildLocalShadowCubeFace(glm::vec3(0.0f), 5.0f, face);
        const glm::vec4 clip = viewProjection * glm::vec4(axes[face] * 2.0f, 1.0f);
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        Require(std::abs(ndc.x) < 1e-4f && std::abs(ndc.y) < 1e-4f, "a face's axis maps to its centre");
        Require(ndc.z > 0.0f && ndc.z < 1.0f, "and in front of the light");
    }
}

void SpotFrustumHoldsTheWholeCone()
{
    LocalShadowLight light = MakeLight(LightType::Spot, glm::vec3(0.0f, 4.0f, -5.0f), 8.0f);
    light.direction = glm::normalize(glm::vec3(0.2f, -1.0f, 0.1f));
    light.outerAngleRadians = glm::radians(40.0f);
    const LocalShadowPlan plan = PlanLocalShadows(std::span(&light, 1), MakeCamera(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
    Require(plan.tiles.size() == 1, "a spot light takes one tile");

    const float limit = 0.5f - kGuard + 1e-4f;
    std::mt19937 random(11);
    const glm::vec3 side = glm::normalize(glm::cross(light.direction, glm::vec3(0.0f, 0.0f, 1.0f)));
    for (int sample = 0; sample < 5000; ++sample)
    {
        // A random direction within the outer cone.
        const float angle = light.outerAngleRadians * std::sqrt(static_cast<float>(sample % 101) / 100.0f);
        const float around = 6.2831853f * static_cast<float>(sample) / 5000.0f;
        const glm::vec3 tilted = glm::rotate(glm::mat4(1.0f), angle, side) * glm::vec4(light.direction, 0.0f);
        const glm::vec3 direction = glm::rotate(glm::mat4(1.0f), around, light.direction) * glm::vec4(tilted, 0.0f);
        const float distance = 0.1f + 7.8f * static_cast<float>(sample % 13) / 12.0f;
        const glm::vec3 local = TileLocal(plan.tiles[0], light.position + direction * distance);
        Require(std::abs(local.x - 0.5f) <= limit && std::abs(local.y - 0.5f) <= limit, "the cone fits inside the guard band");
        Require(local.z > 0.0f && local.z < 1.0f, "the cone lies between the near and far planes");
    }

    light.outerAngleRadians = glm::radians(75.0f);
    const LocalShadowPlan wide = PlanLocalShadows(std::span(&light, 1), MakeCamera(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
    Require(wide.tiles.size() == kLocalShadowCubeFaceCount, "a spot wider than 60 degrees is shadowed as a point");
}

void TilesAreHandedOutInOrderUntilTheyRunOut()
{
    const glm::mat4 camera = MakeCamera(glm::vec3(0.0f, 2.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    std::vector<LocalShadowLight> lights;
    lights.push_back(MakeLight(LightType::Point, glm::vec3(0.0f, 2.0f, 0.0f)));
    LocalShadowLight noShadow = MakeLight(LightType::Point, glm::vec3(1.0f, 2.0f, 0.0f));
    noShadow.castShadows = false;
    lights.push_back(noShadow);
    lights.push_back(MakeLight(LightType::Point, glm::vec3(0.0f, 2.0f, 100.0f), 5.0f)); // behind the camera
    lights.push_back(MakeLight(LightType::Directional, glm::vec3(0.0f)));
    lights.push_back(MakeLight(LightType::Area, glm::vec3(-1.0f, 2.0f, 0.0f)));
    for (int i = 0; i < 12; ++i)
    {
        lights.push_back(MakeLight(LightType::Point, glm::vec3(static_cast<float>(i), 1.0f, -2.0f)));
    }
    LocalShadowLight spot = MakeLight(LightType::Spot, glm::vec3(0.0f, 3.0f, -1.0f));
    spot.outerAngleRadians = glm::radians(30.0f);
    lights.push_back(spot);

    const LocalShadowPlan plan = PlanLocalShadows(lights, camera);
    Require(plan.firstTile.size() == lights.size(), "one entry per light");
    Require(plan.firstTile[0] == 0, "the first light gets the first tiles");
    Require(plan.firstTile[1] == -1, "a light without shadows gets no tile");
    Require(plan.firstTile[2] == -1, "a light out of view gets no tile");
    Require(plan.firstTile[3] == -1, "a directional light gets no atlas tile");
    Require(plan.firstTile[4] == 6, "an area light takes six tiles after the first light's");
    // 64 tiles: 2 cubes so far, then 8 more cubes fit (60 tiles), 4 tiles remain.
    for (int i = 0; i < 12; ++i)
    {
        const int32_t expected = i < 8 ? 12 + 6 * i : -1;
        Require(plan.firstTile[5 + static_cast<size_t>(i)] == expected, "cubes are served in order until the atlas is full");
    }
    Require(plan.firstTile.back() == 60, "a later spot still fits into the remaining tiles");
    Require(plan.droppedCount == 4, "the cubes that did not fit are counted");
    Require(plan.tiles.size() == 61, "every handed out tile is listed");

    // No two tiles share a texel, and every tile lies inside the atlas.
    for (size_t a = 0; a < plan.tiles.size(); ++a)
    {
        const glm::uvec2 offset = plan.tiles[a].atlasOffsetTexels;
        Require(offset.x + kLocalShadowTileSize <= kLocalShadowAtlasSize && offset.y + kLocalShadowTileSize <= kLocalShadowAtlasSize,
                "a tile lies inside the atlas");
        const glm::vec4 rect = plan.tiles[a].atlasRect;
        Require(std::abs(rect.x - offset.x / static_cast<float>(kLocalShadowAtlasSize)) < 1e-6f &&
                    std::abs(rect.z - kLocalShadowTileSize / static_cast<float>(kLocalShadowAtlasSize)) < 1e-6f,
                "the uv rect matches the texel rect");
        for (size_t b = a + 1; b < plan.tiles.size(); ++b)
        {
            Require(plan.tiles[b].atlasOffsetTexels != offset, "no two tiles overlap");
        }
    }
}

void AtlasCoordinatesStayInsideTheTile()
{
    const glm::vec4 rect(0.25f, 0.5f, 0.125f, 0.125f);
    const glm::vec3 centre = shader::LocalShadowAtlasCoordinates(glm::vec4(0.0f, 0.0f, 0.5f, 2.0f), rect, kGuard);
    Require(std::abs(centre.x - 0.3125f) < 1e-6f && std::abs(centre.y - 0.5625f) < 1e-6f, "the clip centre is the tile centre");
    Require(std::abs(centre.z - 0.25f) < 1e-6f, "depth is divided by w");
    const glm::vec3 outside = shader::LocalShadowAtlasCoordinates(glm::vec4(-5.0f, 9.0f, 0.5f, 1.0f), rect, kGuard);
    Require(outside.x >= rect.x + rect.z * kGuard - 1e-6f && outside.y <= rect.y + rect.w * (1.0f - kGuard) + 1e-6f,
            "a point off the tile is clamped inside its guard band");
}

void FrustumSphereTest()
{
    const glm::mat4 camera = MakeCamera(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    Require(FrustumIntersectsSphere(camera, glm::vec3(0.0f, 0.0f, -10.0f), 0.1f), "a sphere ahead is inside");
    Require(!FrustumIntersectsSphere(camera, glm::vec3(0.0f, 0.0f, 10.0f), 1.0f), "a sphere behind is outside");
    Require(FrustumIntersectsSphere(camera, glm::vec3(0.0f, 0.0f, 0.5f), 1.0f), "a sphere around the eye touches the near plane");
    Require(!FrustumIntersectsSphere(camera, glm::vec3(0.0f, 0.0f, -300.0f), 50.0f), "a sphere beyond the far plane is outside");
    Require(FrustumIntersectsSphere(camera, glm::vec3(0.0f, 0.0f, -300.0f), 150.0f), "one reaching back past the far plane is inside");
    // The left plane at 10 m is about 10 tan(30 deg) * 16/9 = 10.26 m to the side.
    Require(!FrustumIntersectsSphere(camera, glm::vec3(-12.0f, 0.0f, -10.0f), 1.0f), "a sphere past the left plane is outside");
    Require(FrustumIntersectsSphere(camera, glm::vec3(-12.0f, 0.0f, -10.0f), 2.0f), "a sphere reaching over the left plane is inside");
    Require(!FrustumIntersectsSphere(camera, glm::vec3(0.0f, 7.0f, -10.0f), 0.5f), "a sphere above the top plane is outside");
}
}

int main()
{
    try
    {
        CubeFacesCoverEveryDirectionWithTheGuardToSpare();
        CubeFacesFollowTheSelectionOrder();
        SpotFrustumHoldsTheWholeCone();
        TilesAreHandedOutInOrderUntilTheyRunOut();
        AtlasCoordinatesStayInsideTheTile();
        FrustumSphereTest();
    }
    catch (const std::exception& error)
    {
        std::cerr << "local shadows tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "local shadows tests passed\n";
    return 0;
}
