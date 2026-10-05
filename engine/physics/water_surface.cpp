#include "water_surface.h"

#include <algorithm>
#include <cmath>

namespace me
{

WaterSurface::WaterSurface(float cellSize)
    : m_cellSize(std::max(cellSize, 1.0f))
{
}

uint64_t WaterSurface::CellKey(int32_t x, int32_t z)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
}

void WaterSurface::Add(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices)
{
    for (size_t index = 0; index + 2 < indices.size(); index += 3)
    {
        if (indices[index] >= vertices.size() || indices[index + 1] >= vertices.size() || indices[index + 2] >= vertices.size())
        {
            continue;
        }
        const Triangle triangle{vertices[indices[index]], vertices[indices[index + 1]], vertices[indices[index + 2]]};
        // Seen from above it must cover some ground: a wall of water has no top.
        const float area = (triangle.b.x - triangle.a.x) * (triangle.c.z - triangle.a.z) - (triangle.c.x - triangle.a.x) * (triangle.b.z - triangle.a.z);
        if (std::abs(area) < 1e-6f)
        {
            continue;
        }
        const uint32_t triangleIndex = static_cast<uint32_t>(m_triangles.size());
        m_triangles.push_back(triangle);
        const float minX = std::min({triangle.a.x, triangle.b.x, triangle.c.x});
        const float maxX = std::max({triangle.a.x, triangle.b.x, triangle.c.x});
        const float minZ = std::min({triangle.a.z, triangle.b.z, triangle.c.z});
        const float maxZ = std::max({triangle.a.z, triangle.b.z, triangle.c.z});
        const int32_t cellMinX = static_cast<int32_t>(std::floor(minX / m_cellSize));
        const int32_t cellMaxX = static_cast<int32_t>(std::floor(maxX / m_cellSize));
        const int32_t cellMinZ = static_cast<int32_t>(std::floor(minZ / m_cellSize));
        const int32_t cellMaxZ = static_cast<int32_t>(std::floor(maxZ / m_cellSize));
        for (int32_t cellX = cellMinX; cellX <= cellMaxX; ++cellX)
        {
            for (int32_t cellZ = cellMinZ; cellZ <= cellMaxZ; ++cellZ)
            {
                m_cells[CellKey(cellX, cellZ)].push_back(triangleIndex);
            }
        }
    }
}

bool WaterSurface::Empty() const
{
    return m_triangles.empty();
}

size_t WaterSurface::TriangleCount() const
{
    return m_triangles.size();
}

std::optional<float> WaterSurface::HeightAt(float x, float z) const
{
    const auto cell = m_cells.find(CellKey(static_cast<int32_t>(std::floor(x / m_cellSize)), static_cast<int32_t>(std::floor(z / m_cellSize))));
    if (cell == m_cells.end())
    {
        return std::nullopt;
    }
    std::optional<float> highest;
    for (uint32_t triangleIndex : cell->second)
    {
        const Triangle& t = m_triangles[triangleIndex];
        const float denominator = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
        const float u = ((t.b.z - t.c.z) * (x - t.c.x) + (t.c.x - t.b.x) * (z - t.c.z)) / denominator;
        const float v = ((t.c.z - t.a.z) * (x - t.c.x) + (t.a.x - t.c.x) * (z - t.c.z)) / denominator;
        const float w = 1.0f - u - v;
        constexpr float kEdge = -1e-5f; // a point on a shared edge belongs to both triangles
        if (u < kEdge || v < kEdge || w < kEdge)
        {
            continue;
        }
        const float height = u * t.a.y + v * t.b.y + w * t.c.y;
        highest = highest.has_value() ? std::max(*highest, height) : height;
    }
    return highest;
}
}
