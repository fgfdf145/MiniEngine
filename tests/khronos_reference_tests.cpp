#include <engine/renderer/camera.h>
#include <engine/renderer/exposure.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// The PBR Neutral operator the tone mapping pass runs, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/pbr_neutral.glsl>
}

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

// Khronos PBR Neutral as published (KhronosGroup/ToneMapping, PBR_Neutral), written out here
// independently of the shader.
glm::vec3 Reference(glm::vec3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;
    const float x = std::min({color.r, color.g, color.b});
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;
    const float peak = std::max({color.r, color.g, color.b});
    if (peak < startCompression)
    {
        return color;
    }
    const float d = 1.0f - startCompression;
    const float newPeak = 1.0f - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return color * (1.0f - g) + glm::vec3(newPeak) * g;
}

bool Near(const glm::vec3& actual, const glm::vec3& expected, float tolerance = 1e-5f)
{
    return glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(tolerance)));
}

std::string Text(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

void PbrNeutralMatchesKhronos()
{
    const glm::vec3 inputs[] = {
        glm::vec3(0.0f),
        glm::vec3(0.18f),
        glm::vec3(0.05f, 0.2f, 0.5f),
        glm::vec3(5.0f, 2.0f, 1.0f),
        glm::vec3(100.0f, 0.5f, 0.02f),
        glm::vec3(0.8f)};
    for (const glm::vec3& input : inputs)
    {
        const glm::vec3 port = shader::KhronosPbrNeutral(input);
        Require(Near(port, Reference(input)), "PBR Neutral of " + Text(input) + " is " + Text(port) + ", not " + Text(Reference(input)));
    }
    Require(Near(shader::KhronosPbrNeutral(glm::vec3(0.0f)), glm::vec3(0.0f)), "black is not black");
    Require(Near(shader::KhronosPbrNeutral(glm::vec3(0.18f)), glm::vec3(0.14f)), "mid grey is not offset by 0.04");
    const glm::vec3 bright = shader::KhronosPbrNeutral(glm::vec3(5.0f, 2.0f, 1.0f));
    Require(bright.r < 1.0f && bright.r > 0.9f, "a bright colour is not compressed below 1");
    Require(bright.b / bright.r > (1.0f - 0.04f) / (5.0f - 0.04f), "a bright colour is not desaturated");
    // Continuous where compression starts: the peak there maps to itself.
    const float start = 0.8f - 0.04f;
    const glm::vec3 atStart = shader::KhronosPbrNeutral(glm::vec3(start + 0.04f));
    Require(std::abs(atStart.r - start) < 1e-4f, "the curve jumps where compression starts");
}

// The Sample Viewer's rule: target the box centre, look down -Z, back off until the larger of the
// x and y extents fits the vertical FOV and the horizontal one (vertical FOV times aspect).
float ExpectedDistance(float width, float height, float aspect)
{
    const float yfov = glm::radians(45.0f);
    const float xfov = yfov * aspect;
    const float maxAxis = std::max(width, height);
    return std::max(maxAxis / 2.0f / std::tan(yfov / 2.0f), maxAxis / 2.0f / std::tan(xfov / 2.0f));
}

void FramesLikeTheSampleViewer()
{
    Camera camera;
    camera.FrameBoundsLikeKhronosViewer(glm::vec3(-1.0f), glm::vec3(1.0f), 1.5f);
    Require(std::abs(camera.fovDegrees - 45.0f) < 1e-5f, "the viewer's vertical FOV is 45 degrees");
    Require(Near(camera.GetForward(), glm::vec3(0.0f, 0.0f, -1.0f)), "the camera does not look down -Z");
    Require(Near(camera.position, glm::vec3(0.0f, 0.0f, ExpectedDistance(2.0f, 2.0f, 1.5f)), 1e-4f),
            "a unit cube is not framed from " + std::to_string(ExpectedDistance(2.0f, 2.0f, 1.5f)) + ": " + Text(camera.position));

    // The distance is measured from the centre; depth does not enter it.
    camera.FrameBoundsLikeKhronosViewer(glm::vec3(1.0f, 2.0f, -10.0f), glm::vec3(5.0f, 3.0f, 10.0f), 1.5f);
    Require(Near(camera.position, glm::vec3(3.0f, 2.5f, ExpectedDistance(4.0f, 1.0f, 1.5f)), 1e-4f),
            "a wide box is not framed by its width: " + Text(camera.position));

    // A narrow viewport: the horizontal FOV decides.
    camera.FrameBoundsLikeKhronosViewer(glm::vec3(0.0f), glm::vec3(1.0f, 3.0f, 1.0f), 0.5f);
    Require(Near(camera.position, glm::vec3(0.5f, 1.5f, 0.5f + ExpectedDistance(1.0f, 3.0f, 0.5f)), 1e-4f),
            "a tall box in a narrow view is not framed by the horizontal FOV: " + Text(camera.position));
    Require(camera.nearPlane > 0.0f && camera.nearPlane < ExpectedDistance(1.0f, 3.0f, 0.5f) - 1.0f, "the near plane clips the model");
    Require(camera.farPlane > ExpectedDistance(1.0f, 3.0f, 0.5f) + 3.0f, "the far plane clips the model");
}

void ExposesHdriTexelOneToOne()
{
    for (const float intensity : {1.0f, 100.0f, 1000.0f, 30000.0f})
    {
        const float ev = KhronosReferenceEv100(intensity);
        const float exposed = intensity * ExposureFromEv100(ev);
        Require(std::abs(exposed - 1.0f) < 1e-4f, "an HDRI texel of 1 at intensity " + std::to_string(intensity) + " is exposed to " + std::to_string(exposed));
    }
}
}

int main()
{
    try
    {
        PbrNeutralMatchesKhronos();
        FramesLikeTheSampleViewer();
        ExposesHdriTexelOneToOne();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Khronos reference tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Khronos reference tests passed\n";
    return 0;
}
