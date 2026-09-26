#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// The volume scatter rules the forward shader uses, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/volume_scatter_common.glsl>
}

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearRelative(float value, float expected, float tolerance = 1e-3f)
{
    return std::abs(value - expected) <= tolerance * std::max(std::abs(expected), 1e-6f);
}

// The extension's mapping: no multi-scatter albedo is no scattering, a white one all scattering.
void SingleScatterFollowsTheExtension()
{
    const glm::vec3 none = shader::MultiToSingleScatter(glm::vec3(0.0f));
    Require(std::abs(none.x) < 1e-4f, "multi-scatter 0 is single-scatter 0");
    const glm::vec3 white = shader::MultiToSingleScatter(glm::vec3(1.0f));
    Require(std::abs(white.x - 1.0f) < 1e-4f, "multi-scatter 1 is single-scatter 1");
    // ScatteringSkull's colour, against the formula in double precision.
    const glm::vec3 skull = shader::MultiToSingleScatter(glm::vec3(0.16827136f, 0.52711987f, 0.59062535f));
    Require(NearRelative(skull.x, 0.55216651f) && NearRelative(skull.y, 0.92419154f) && NearRelative(skull.z, 0.94818462f),
            "the skull's single-scatter albedo");
}

// The samples, against the Sample Viewer's computeScatterSamples (gltf/material.js) evaluated in double
// precision: angle, radius and one over the pdf.
void SamplesMatchTheViewer()
{
    struct Expected
    {
        int index;
        float theta;
        float radius;
        float reciprocalPdf;
    };
    const Expected expected[] = {
        {0, 3.14159265f, 0.00139460673f, 0.96975098f},
        {1, 5.54155588f, 0.00423547238f, 0.99390251f},
        {27, 67.9405999f, 0.118083375f, 2.36108009f},
        {54, 132.739607f, 0.936605542f, 113.316758f}};
    for (const Expected& sample : expected)
    {
        const glm::vec3 value = shader::BurleyScatterSample(sample.index);
        const std::string name = "sample " + std::to_string(sample.index);
        Require(NearRelative(value.x, sample.theta, 1e-5f), name + " angle " + std::to_string(value.x));
        Require(NearRelative(value.y, sample.radius), name + " radius " + std::to_string(value.y));
        Require(NearRelative(value.z, sample.reciprocalPdf), name + " 1/pdf " + std::to_string(value.z));
    }
    Require(NearRelative(shader::BurleyMinimumRadius(), 0.00139460673f), "the minimum radius is the first sample's");
    for (int index = 1; index < shader::kScatterSampleCount; ++index)
    {
        Require(shader::BurleyScatterSample(index).y > shader::BurleyScatterSample(index - 1).y, "the radii grow with the index");
    }
}

// The profile falls with distance and widens with the radius.
void ProfileFallsOff()
{
    const glm::vec3 d = shader::BurleyShape(glm::vec3(1.0f, 2.0f, 4.0f));
    const glm::vec3 near = shader::BurleyProfile(d, 0.01f);
    const glm::vec3 far = shader::BurleyProfile(d, 0.5f);
    Require(far.x < near.x && far.y < near.y && far.z < near.z, "the profile falls with distance");
    Require(far.z / near.z > far.x / near.x, "a wider radius falls more slowly");
    Require(NearRelative(d.x, 0.25f / 3.14159265f / 1.04f), "Burley's shape at a white albedo");
}
}

int main()
{
    try
    {
        SingleScatterFollowsTheExtension();
        SamplesMatchTheViewer();
        ProfileFallsOff();
    }
    catch (const std::exception& error)
    {
        std::cerr << "volume scatter tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "volume scatter tests passed\n";
    return 0;
}
