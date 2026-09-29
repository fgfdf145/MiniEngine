#include "collision_filter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace me
{

namespace
{
// A triangle is upright when its unit normal's |y| is below this: within 30 degrees of vertical.
constexpr float kUprightMaxNormalY = 0.5f;
}

bool IsGroundCover(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices)
{
    size_t triangleCount = 0;
    std::vector<float> uprightHeights;
    for (size_t index = 0; index + 2 < indices.size(); index += 3)
    {
        const uint32_t a = indices[index];
        const uint32_t b = indices[index + 1];
        const uint32_t c = indices[index + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
        {
            continue;
        }
        const glm::vec3 normal = glm::cross(vertices[b] - vertices[a], vertices[c] - vertices[a]);
        const float length = glm::length(normal);
        if (!(length > 1e-12f))
        {
            continue;
        }
        ++triangleCount;
        if (std::abs(normal.y) / length < kUprightMaxNormalY)
        {
            const float low = std::min({vertices[a].y, vertices[b].y, vertices[c].y});
            const float high = std::max({vertices[a].y, vertices[b].y, vertices[c].y});
            uprightHeights.push_back(high - low);
        }
    }
    if (triangleCount == 0 || static_cast<float>(uprightHeights.size()) < kGroundCoverMinUprightShare * static_cast<float>(triangleCount))
    {
        return false;
    }

    const auto middle = uprightHeights.begin() + static_cast<std::ptrdiff_t>(uprightHeights.size() / 2);
    std::nth_element(uprightHeights.begin(), middle, uprightHeights.end());
    return *middle < kGroundCoverMaxTriangleHeight;
}
}
