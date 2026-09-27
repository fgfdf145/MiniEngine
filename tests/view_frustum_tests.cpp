#include <engine/renderer/view_frustum.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A camera at the origin looking down -Z, 90 degrees vertically, square, near 0.1 and far 100, with
// the Y flip the renderer applies for Vulkan.
glm::mat4 MakeViewProjection()
{
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    projection[1][1] *= -1.0f;
    return projection * glm::lookAtRH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

void KeepsSpheresInside()
{
    const ViewFrustum frustum(MakeViewProjection());
    Require(frustum.IntersectsSphere(glm::vec3(0.0f, 0.0f, -10.0f), 1.0f), "a sphere ahead is kept");
    Require(frustum.IntersectsSphere(glm::vec3(9.0f, 9.0f, -10.0f), 0.5f), "a sphere near a corner is kept");
}

void RejectsSpheresOutside()
{
    const ViewFrustum frustum(MakeViewProjection());
    Require(!frustum.IntersectsSphere(glm::vec3(0.0f, 0.0f, 10.0f), 1.0f), "a sphere behind is culled");
    Require(!frustum.IntersectsSphere(glm::vec3(20.0f, 0.0f, -10.0f), 1.0f), "a sphere to the right is culled");
    Require(!frustum.IntersectsSphere(glm::vec3(0.0f, -20.0f, -10.0f), 1.0f), "a sphere below is culled");
    Require(!frustum.IntersectsSphere(glm::vec3(0.0f, 0.0f, -150.0f), 1.0f), "a sphere past the far plane is culled");
}

void KeepsSpheresStraddlingAPlane()
{
    const ViewFrustum frustum(MakeViewProjection());
    // Centre 1 m right of the right plane (x = -z), radius 2: part of it is inside.
    const float outside = 10.0f + 1.0f * std::sqrt(2.0f);
    Require(frustum.IntersectsSphere(glm::vec3(outside, 0.0f, -10.0f), 2.0f), "a straddling sphere is kept");
    Require(frustum.IntersectsSphere(glm::vec3(0.0f, 0.0f, 0.5f), 1.0f), "a sphere around the camera is kept");
}
}

int main()
{
    try
    {
        KeepsSpheresInside();
        RejectsSpheresOutside();
        KeepsSpheresStraddlingAPlane();
    }
    catch (const std::exception& error)
    {
        std::cerr << "view_frustum_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "view_frustum_tests passed\n";
    return 0;
}
