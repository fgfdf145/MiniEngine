#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

// The top of the world's water, seen from above (Y up): triangles in world space, looked up by where
// a point stands on the ground plane. Where surfaces overlap, the highest one counts. A grid of square
// cells keeps a lookup to the few triangles over the point.
class WaterSurface
{
  public:
    explicit WaterSurface(float cellSize = 64.0f);

    void Add(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices);
    bool Empty() const;
    size_t TriangleCount() const;

    // The water's height over (x, z), or nothing where there is no water.
    std::optional<float> HeightAt(float x, float z) const;

  private:
    struct Triangle
    {
        glm::vec3 a;
        glm::vec3 b;
        glm::vec3 c;
    };
    static uint64_t CellKey(int32_t x, int32_t z);

    float m_cellSize;
    std::vector<Triangle> m_triangles;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_cells;
};
}
