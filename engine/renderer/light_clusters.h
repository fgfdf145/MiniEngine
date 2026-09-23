#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// The froxel grid the local lights are binned into: 16 x 9 tiles across NDC, whatever the viewport
// size, times 24 depth slices spaced exponentially between the camera's near and far planes. Must
// match the LIGHT_CLUSTER_* constants in shaders/vulkan/scene_common.glsl.
inline constexpr uint32_t kLightClusterTilesX = 16;
inline constexpr uint32_t kLightClusterTilesY = 9;
inline constexpr uint32_t kLightClusterSlices = 24;
inline constexpr uint32_t kLightClusterCount = kLightClusterTilesX * kLightClusterTilesY * kLightClusterSlices;
// Entries in the flattened per-cluster light index list, 512 KiB of uints. Far above what 1024
// lights of room-sized range need.
inline constexpr uint32_t kLightClusterIndexCapacity = 131072;

struct LightClusterCamera
{
    glm::mat4 view{1.0f};
    // The render projection the shaders use, Y flip included: the shader finds a pixel's tile
    // through this same matrix, so binning against anything else would disagree with it.
    glm::mat4 projection{1.0f};
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

// The volume a light can reach: its range around its position (an area light's centre). The light
// contributes exactly zero outside it (see RangeWindow in pbr_common.glsl), which is what lets a
// cluster leave it out.
struct LightClusterSphere
{
    glm::vec3 worldCenter{0.0f};
    float radius = 0.0f;
    // Index into the uploaded light array.
    uint32_t lightIndex = 0;
};

struct LightClusterGrid
{
    // One (offset, count) per cluster into indices, cluster index = (slice * tilesY + tileY) *
    // tilesX + tileX. Always kLightClusterCount entries.
    std::vector<glm::uvec2> ranges;
    // The light indices of every cluster, one cluster after another. Within a cluster they keep the
    // order of the spheres passed in, so ascending input gives ascending lists.
    std::vector<uint32_t> indices;
    // slice = floor(log(viewDepth) * sliceScale + sliceBias), clamped to the grid.
    float sliceScale = 0.0f;
    float sliceBias = 0.0f;
    // (light, cluster) pairs left out because the index list was full.
    uint32_t droppedCount = 0;
};

// Bins every sphere into each cluster it might overlap. Conservative: a sphere is never left out of
// a cluster it reaches into, though it may be listed in a neighbour it only nearly reaches.
LightClusterGrid BuildLightClusters(
    const LightClusterCamera& camera,
    std::span<const LightClusterSphere> spheres,
    uint32_t indexCapacity);

// The cluster a view-space position falls in, by the arithmetic FindLightCluster in pbr_common.glsl
// runs. Positions outside the grid clamp to its edge clusters.
uint32_t FindLightCluster(const LightClusterGrid& grid, const glm::mat4& projection, const glm::vec3& viewPosition);
}
