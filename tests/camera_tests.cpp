#include <engine/renderer/camera.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

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

// The pivot in view space: where it is on screen (x and y over -z) and how far away.
glm::vec3 ViewSpace(const Camera& camera, const glm::vec3& point)
{
    return glm::vec3(camera.GetViewMatrix() * glm::vec4(point, 1.0f));
}

// Orbiting turns the view and swings the camera around the pivot together: the pivot stays where it
// was on screen, at the same distance, even when it is off the centre of the view.
void OrbitKeepsThePivotInPlace()
{
    Camera camera;
    camera.position = glm::vec3(1.0f, 2.0f, 6.0f);
    camera.yawDegrees = -100.0f;
    camera.pitchDegrees = -10.0f;
    const glm::vec3 pivot(0.5f, 0.0f, 0.0f);
    const glm::vec3 before = ViewSpace(camera, pivot);

    camera.Orbit(pivot, 35.0f, 20.0f);
    const glm::vec3 after = ViewSpace(camera, pivot);
    Require(glm::length(after - before) < 1e-4f, "the pivot stays put in view space");
    Require(std::abs(camera.yawDegrees - -65.0f) < 1e-4f && std::abs(camera.pitchDegrees - 10.0f) < 1e-4f,
            "the view turns as Rotate turns it");
}

// Past the pitch limit the view stops turning, and so does the camera's swing around the pivot.
void OrbitStopsAtThePitchLimit()
{
    Camera camera;
    camera.position = glm::vec3(0.0f, 0.0f, 4.0f);
    const glm::vec3 pivot(0.0f);
    const glm::vec3 before = ViewSpace(camera, pivot);

    camera.Orbit(pivot, 0.0f, 120.0f);
    Require(std::abs(camera.pitchDegrees - 89.0f) < 1e-4f, "the pitch is clamped");
    Require(glm::length(ViewSpace(camera, pivot) - before) < 1e-4f, "the pivot still stays put");
}
}

int main()
{
    try
    {
        OrbitKeepsThePivotInPlace();
        OrbitStopsAtThePitchLimit();
    }
    catch (const std::exception& error)
    {
        std::cerr << "camera tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "camera tests passed\n";
    return 0;
}
