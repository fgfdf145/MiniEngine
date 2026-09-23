#include <engine/renderer/camera.h>
#include <engine/renderer/light_clusters.h>

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
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

// The camera the renderer uses: Y flipped, zero-to-one depth.
LightClusterCamera MakeCamera(const Camera& camera, float aspect, const glm::mat4& view)
{
    LightClusterCamera clusterCamera{};
    clusterCamera.view = view;
    clusterCamera.projection = camera.GetProjectionMatrix(
        RenderExtent{static_cast<uint32_t>(1000.0f * aspect), 1000u},
        true,
        true);
    clusterCamera.nearPlane = camera.nearPlane;
    clusterCamera.farPlane = camera.farPlane;
    return clusterCamera;
}

bool ClusterListsLight(const LightClusterGrid& grid, uint32_t cluster, uint32_t lightIndex)
{
    const glm::uvec2 range = grid.ranges[cluster];
    for (uint32_t k = 0; k < range.y; ++k)
    {
        if (grid.indices[range.x + k] == lightIndex)
        {
            return true;
        }
    }
    return false;
}

// The property everything else rests on: a point within a light's range is always in a cluster
// that lists the light. Skipping a light is then only ever skipping an exact zero.
void ConservativeBinning()
{
    std::mt19937 rng(1234u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto range = [&](float low, float high)
    {
        return low + (high - low) * unit(rng);
    };
    const auto direction = [&]()
    {
        glm::vec3 v;
        do
        {
            v = glm::vec3(range(-1.0f, 1.0f), range(-1.0f, 1.0f), range(-1.0f, 1.0f));
        } while (glm::dot(v, v) > 1.0f || glm::dot(v, v) < 1e-4f);
        return glm::normalize(v);
    };

    uint32_t checkedPoints = 0;
    for (int cameraIndex = 0; cameraIndex < 200; ++cameraIndex)
    {
        Camera camera{};
        camera.fovDegrees = range(30.0f, 90.0f);
        camera.nearPlane = range(0.05f, 1.0f);
        camera.farPlane = range(20.0f, 500.0f);
        const float aspect = range(0.3f, 3.0f);
        const glm::vec3 eye(range(-20.0f, 20.0f), range(-5.0f, 5.0f), range(-20.0f, 20.0f));
        const glm::vec3 forward = direction();
        const glm::vec3 up = std::fabs(forward.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 view = glm::lookAt(eye, eye + forward, up);
        const LightClusterCamera clusterCamera = MakeCamera(camera, aspect, view);

        std::vector<LightClusterSphere> spheres;
        for (uint32_t lightIndex = 0; lightIndex < 40; ++lightIndex)
        {
            LightClusterSphere sphere{};
            // A quarter of the lights sit around the eye, so spheres containing the camera and
            // spheres straddling the near plane are common rather than rare.
            const float distance = lightIndex % 4 == 0 ? range(0.0f, 2.0f) : range(0.0f, 60.0f);
            sphere.worldCenter = eye + direction() * distance;
            sphere.radius = range(0.2f, 15.0f);
            sphere.lightIndex = lightIndex;
            spheres.push_back(sphere);
        }
        const LightClusterGrid grid = BuildLightClusters(clusterCamera, spheres, kLightClusterIndexCapacity);
        Require(grid.droppedCount == 0, "40 lights fit the default capacity");

        for (const LightClusterSphere& sphere : spheres)
        {
            for (int pointIndex = 0; pointIndex < 50; ++pointIndex)
            {
                const glm::vec3 world = sphere.worldCenter + direction() * (sphere.radius * 0.98f * std::cbrt(unit(rng)));
                const glm::vec3 viewPosition(view * glm::vec4(world, 1.0f));
                const float depth = -viewPosition.z;
                if (depth < camera.nearPlane || depth > camera.farPlane)
                {
                    continue;
                }
                const glm::vec4 clip = clusterCamera.projection * glm::vec4(viewPosition, 1.0f);
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f)
                {
                    continue;
                }
                const uint32_t cluster = FindLightCluster(grid, clusterCamera.projection, viewPosition);
                Require(cluster < kLightClusterCount, "the lookup stays inside the grid");
                Require(
                    ClusterListsLight(grid, cluster, sphere.lightIndex),
                    "camera " + std::to_string(cameraIndex) + ": light " + std::to_string(sphere.lightIndex) +
                        " reaches a point in cluster " + std::to_string(cluster) + " that does not list it");
                ++checkedPoints;
            }
        }
    }
    Require(checkedPoints > 20000, "enough points landed in the frustum to mean something, got " + std::to_string(checkedPoints));
}

void CullsOutsideTheFrustumDepth()
{
    Camera camera{};
    camera.nearPlane = 0.1f;
    camera.farPlane = 50.0f;
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const LightClusterCamera clusterCamera = MakeCamera(camera, 16.0f / 9.0f, view);

    const std::vector<LightClusterSphere> spheres = {
        LightClusterSphere{glm::vec3(0.0f, 0.0f, 5.0f), 2.0f, 0u},   // wholly behind the eye
        LightClusterSphere{glm::vec3(0.0f, 0.0f, -60.0f), 5.0f, 1u}, // wholly beyond far
    };
    const LightClusterGrid grid = BuildLightClusters(clusterCamera, spheres, kLightClusterIndexCapacity);
    Require(grid.indices.empty(), "lights outside the depth range are in no cluster");
    Require(grid.ranges.size() == kLightClusterCount, "every cluster has a range, empty or not");
}

void IndicesAscendAndRangesTile()
{
    Camera camera{};
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const LightClusterCamera clusterCamera = MakeCamera(camera, 16.0f / 9.0f, view);

    std::vector<LightClusterSphere> spheres;
    for (uint32_t i = 0; i < 64; ++i)
    {
        const float angle = static_cast<float>(i) * 0.7f;
        spheres.push_back(LightClusterSphere{
            glm::vec3(std::cos(angle) * 3.0f, 0.5f, std::sin(angle) * 3.0f),
            1.0f + static_cast<float>(i % 5),
            i});
    }
    const LightClusterGrid grid = BuildLightClusters(clusterCamera, spheres, kLightClusterIndexCapacity);
    Require(!grid.indices.empty(), "lights in front of the camera land somewhere");

    uint32_t expectedOffset = 0;
    for (uint32_t cluster = 0; cluster < kLightClusterCount; ++cluster)
    {
        const glm::uvec2 range = grid.ranges[cluster];
        Require(range.x == expectedOffset, "ranges follow one another without gaps");
        for (uint32_t k = 1; k < range.y; ++k)
        {
            Require(grid.indices[range.x + k - 1] < grid.indices[range.x + k], "indices ascend within a cluster");
        }
        expectedOffset += range.y;
    }
    Require(expectedOffset == grid.indices.size(), "the ranges cover the whole index list");
}

void CapacityIsHonoured()
{
    Camera camera{};
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const LightClusterCamera clusterCamera = MakeCamera(camera, 16.0f / 9.0f, view);

    // Each covers the whole frustum, so each lands in every cluster.
    std::vector<LightClusterSphere> spheres;
    for (uint32_t i = 0; i < 20; ++i)
    {
        spheres.push_back(LightClusterSphere{glm::vec3(0.0f), 1000.0f, i});
    }
    const LightClusterGrid grid = BuildLightClusters(clusterCamera, spheres, 1000u);
    Require(grid.indices.size() == 1000u, "the index list stops at the capacity, got " + std::to_string(grid.indices.size()));
    Require(
        grid.droppedCount == 20u * kLightClusterCount - 1000u,
        "every pair left out is counted, got " + std::to_string(grid.droppedCount));
    uint32_t listed = 0;
    for (const glm::uvec2& range : grid.ranges)
    {
        Require(range.x + range.y <= grid.indices.size(), "no range points past the list");
        listed += range.y;
    }
    Require(listed == 1000u, "the ranges account for exactly the stored indices");
}

void SlicesAreMonotonic()
{
    Camera camera{};
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.0f;
    const glm::mat4 view(1.0f);
    const LightClusterCamera clusterCamera = MakeCamera(camera, 16.0f / 9.0f, view);
    const LightClusterGrid grid = BuildLightClusters(clusterCamera, {}, kLightClusterIndexCapacity);

    const auto sliceAt = [&](float depth)
    {
        return FindLightCluster(grid, clusterCamera.projection, glm::vec3(0.0f, 0.0f, -depth)) /
               (kLightClusterTilesX * kLightClusterTilesY);
    };
    Require(sliceAt(camera.nearPlane) == 0u, "the near plane is slice 0");
    Require(sliceAt(camera.farPlane * 0.999f) == kLightClusterSlices - 1, "just short of far is the last slice");
    uint32_t previous = 0;
    for (float depth = camera.nearPlane; depth < camera.farPlane; depth *= 1.01f)
    {
        const uint32_t slice = sliceAt(depth);
        Require(slice >= previous, "slices never go back as depth grows");
        previous = slice;
    }
    Require(previous == kLightClusterSlices - 1, "the sweep reaches the last slice");
}

void DegenerateCameraStaysFinite()
{
    LightClusterCamera clusterCamera{};
    clusterCamera.projection = glm::mat4(1.0f);
    clusterCamera.nearPlane = 0.0f;
    clusterCamera.farPlane = 0.0f;
    const LightClusterGrid grid = BuildLightClusters(clusterCamera, {}, kLightClusterIndexCapacity);
    Require(std::isfinite(grid.sliceScale) && std::isfinite(grid.sliceBias), "slice parameters stay finite");
}
}

int main()
{
    try
    {
        ConservativeBinning();
        CullsOutsideTheFrustumDepth();
        IndicesAscendAndRangesTile();
        CapacityIsHonoured();
        SlicesAreMonotonic();
        DegenerateCameraStaysFinite();
    }
    catch (const std::exception& error)
    {
        std::cerr << "light clusters tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "light clusters tests passed\n";
    return 0;
}
