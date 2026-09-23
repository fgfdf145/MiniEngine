#include "light_clusters.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace me
{

namespace
{
// Keeps both logarithms finite for a camera the editor never produces but a caller could.
constexpr float kMinimumNearPlane = 1e-4f;

uint32_t SliceForDepth(float depth, float sliceScale, float sliceBias)
{
    const float slice = std::floor(std::log(std::max(depth, kMinimumNearPlane)) * sliceScale + sliceBias);
    return static_cast<uint32_t>(std::clamp(slice, 0.0f, static_cast<float>(kLightClusterSlices - 1)));
}

uint32_t TileForNdc(float ndc, uint32_t tileCount)
{
    const float tile = std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(tileCount));
    return static_cast<uint32_t>(std::clamp(tile, 0.0f, static_cast<float>(tileCount - 1)));
}

// The planes through the eye that separate the tiles along one axis, in view space. For a
// perspective projection clip.w = -z, so NDC along the axis exceeds `boundary` exactly where
// P[axis][axis] * v[axis] + (P[2][axis] + boundary) * z > 0: the plane's normal is that
// coefficient vector. Normalised so a dot product with a sphere's centre is a signed distance.
// The sign of P[1][1] carries the Y flip without special casing.
template <uint32_t TileCount>
std::array<glm::vec3, TileCount + 1> BuildBoundaryPlanes(const glm::mat4& projection, int axis)
{
    std::array<glm::vec3, TileCount + 1> planes{};
    for (uint32_t i = 0; i <= TileCount; ++i)
    {
        const float boundary = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(TileCount);
        glm::vec3 normal(0.0f);
        normal[axis] = projection[axis][axis];
        normal.z = projection[2][axis] + boundary;
        planes[i] = glm::normalize(normal);
    }
    return planes;
}

// Which tiles along one axis a sphere may overlap: tile i lies on the positive side of boundary i
// and the negative side of boundary i + 1, so the sphere reaches it unless it lies wholly beyond
// either one.
template <uint32_t TileCount>
std::array<bool, TileCount> FindTouchedTiles(
    const std::array<glm::vec3, TileCount + 1>& planes,
    const glm::vec3& center,
    float radius)
{
    std::array<bool, TileCount> touched{};
    for (uint32_t i = 0; i < TileCount; ++i)
    {
        touched[i] = glm::dot(planes[i], center) >= -radius && glm::dot(planes[i + 1], center) <= radius;
    }
    return touched;
}
}

LightClusterGrid BuildLightClusters(
    const LightClusterCamera& camera,
    std::span<const LightClusterSphere> spheres,
    uint32_t indexCapacity)
{
    LightClusterGrid grid;
    grid.ranges.assign(kLightClusterCount, glm::uvec2(0u));

    const float nearPlane = std::max(camera.nearPlane, kMinimumNearPlane);
    const float farPlane = std::max(camera.farPlane, nearPlane * 1.001f);
    const float logDepthRange = std::log(farPlane / nearPlane);
    grid.sliceScale = static_cast<float>(kLightClusterSlices) / logDepthRange;
    grid.sliceBias = -static_cast<float>(kLightClusterSlices) * std::log(nearPlane) / logDepthRange;

    const auto columnPlanes = BuildBoundaryPlanes<kLightClusterTilesX>(camera.projection, 0);
    const auto rowPlanes = BuildBoundaryPlanes<kLightClusterTilesY>(camera.projection, 1);

    // Every (cluster, light) pair, then a counting sort by cluster. The sort is stable, so each
    // cluster keeps the spheres' order, and it costs one pass where per-cluster vectors would cost
    // thousands of allocations a frame.
    std::vector<glm::uvec2> pairs;
    for (const LightClusterSphere& sphere : spheres)
    {
        const glm::vec3 center(camera.view * glm::vec4(sphere.worldCenter, 1.0f));
        const float depth = -center.z;
        const float radius = sphere.radius;
        if (radius <= 0.0f || depth + radius < nearPlane || depth - radius > farPlane)
        {
            continue;
        }

        const uint32_t firstSlice = SliceForDepth(depth - radius, grid.sliceScale, grid.sliceBias);
        const uint32_t lastSlice = SliceForDepth(depth + radius, grid.sliceScale, grid.sliceBias);
        const auto columns = FindTouchedTiles<kLightClusterTilesX>(columnPlanes, center, radius);
        const auto rows = FindTouchedTiles<kLightClusterTilesY>(rowPlanes, center, radius);

        for (uint32_t slice = firstSlice; slice <= lastSlice; ++slice)
        {
            for (uint32_t row = 0; row < kLightClusterTilesY; ++row)
            {
                if (!rows[row])
                {
                    continue;
                }
                for (uint32_t column = 0; column < kLightClusterTilesX; ++column)
                {
                    if (columns[column])
                    {
                        const uint32_t cluster = (slice * kLightClusterTilesY + row) * kLightClusterTilesX + column;
                        pairs.emplace_back(cluster, sphere.lightIndex);
                    }
                }
            }
        }
    }

    std::vector<uint32_t> counts(kLightClusterCount, 0u);
    for (const glm::uvec2& pair : pairs)
    {
        ++counts[pair.x];
    }

    // Clusters are laid out in order and each keeps what fits of its list; once the capacity is
    // reached the rest are counted as dropped rather than written past it.
    uint32_t offset = 0;
    for (uint32_t cluster = 0; cluster < kLightClusterCount; ++cluster)
    {
        const uint32_t kept = std::min(counts[cluster], indexCapacity - offset);
        grid.ranges[cluster] = glm::uvec2(offset, kept);
        grid.droppedCount += counts[cluster] - kept;
        offset += kept;
    }

    grid.indices.resize(offset);
    std::vector<uint32_t> written(kLightClusterCount, 0u);
    for (const glm::uvec2& pair : pairs)
    {
        const glm::uvec2 range = grid.ranges[pair.x];
        uint32_t& slot = written[pair.x];
        if (slot < range.y)
        {
            grid.indices[range.x + slot] = pair.y;
            ++slot;
        }
    }
    return grid;
}

uint32_t FindLightCluster(const LightClusterGrid& grid, const glm::mat4& projection, const glm::vec3& viewPosition)
{
    const glm::vec4 clip = projection * glm::vec4(viewPosition, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    const uint32_t column = TileForNdc(ndc.x, kLightClusterTilesX);
    const uint32_t row = TileForNdc(ndc.y, kLightClusterTilesY);
    const uint32_t slice = SliceForDepth(-viewPosition.z, grid.sliceScale, grid.sliceBias);
    return (slice * kLightClusterTilesY + row) * kLightClusterTilesX + column;
}
}
