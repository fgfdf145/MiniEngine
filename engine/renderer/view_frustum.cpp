#include "view_frustum.h"

namespace me
{

ViewFrustum::ViewFrustum(const glm::mat4& viewProjection)
{
    // Gribb and Hartmann: each clip-space bound is a combination of the matrix's rows. glm is
    // column major, so row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
    const auto row = [&](int index)
    {
        return glm::vec4(viewProjection[0][index], viewProjection[1][index], viewProjection[2][index], viewProjection[3][index]);
    };
    const glm::vec4 x = row(0);
    const glm::vec4 y = row(1);
    const glm::vec4 z = row(2);
    const glm::vec4 w = row(3);
    // -w <= x <= w, -w <= y <= w, 0 <= z <= w.
    const std::array<glm::vec4, 6> planes = {w + x, w - x, w + y, w - y, z, w - z};
    for (size_t index = 0; index < planes.size(); ++index)
    {
        const float length = glm::length(glm::vec3(planes[index]));
        m_planes[index] = length > 1e-6f ? planes[index] / length : glm::vec4(0.0f);
    }
}

bool ViewFrustum::IntersectsSphere(const glm::vec3& center, float radius) const
{
    for (const glm::vec4& plane : m_planes)
    {
        if (glm::dot(glm::vec3(plane), center) + plane.w < -radius)
        {
            return false;
        }
    }
    return true;
}
}
