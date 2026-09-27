#pragma once

#include <glm/glm.hpp>

#include <array>

namespace me
{

// The six planes of a perspective or orthographic view-projection with 0-to-1 clip depth (Vulkan),
// facing inwards, for culling bounding spheres against a camera.
class ViewFrustum
{
  public:
    explicit ViewFrustum(const glm::mat4& viewProjection);

    // False only when the sphere lies wholly outside one plane: a sphere that straddles a corner
    // is kept, which is conservative and costs a few draws.
    bool IntersectsSphere(const glm::vec3& center, float radius) const;

  private:
    // xyz is a unit normal, w the offset: a point p is inside when dot(xyz, p) + w >= 0. A plane
    // that degenerates (an infinite far plane) is left as zero and never rejects.
    std::array<glm::vec4, 6> m_planes{};
};
}
