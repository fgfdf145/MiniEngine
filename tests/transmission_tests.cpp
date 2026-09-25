#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// The transmission sampling rules the forward shader uses, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/transmission_common.glsl>
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

bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-5f)
{
    return glm::length(a - b) < tolerance;
}

void ThinWallsDoNotRefract()
{
    const glm::vec3 position(1.0f, 2.0f, 3.0f);
    const glm::vec3 exit = shader::TransmissionExitPoint(position, glm::vec3(0, 0, 1), glm::normalize(glm::vec3(1, 0, 1)), 1.5f, 0.0f, glm::vec3(1.0f));
    Require(Near(exit, position), "a thin wall's exit is the surface point");
}

void VolumesRefractThroughTheirThickness()
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    // Normal incidence goes straight in, thickness times the node's scale.
    const glm::vec3 straight = shader::TransmissionExitPoint(glm::vec3(0.0f), N, N, 1.5f, 0.5f, glm::vec3(2.0f));
    Require(Near(straight, glm::vec3(0.0f, 0.0f, -1.0f)), "normal incidence goes straight through");

    // Oblique incidence bends toward the normal by Snell's law.
    const glm::vec3 V = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
    const glm::vec3 exit = shader::TransmissionExitPoint(glm::vec3(0.0f), N, V, 1.5f, 1.0f, glm::vec3(1.0f));
    const float sinOut = std::abs(exit.x) / glm::length(exit);
    Require(std::abs(sinOut - std::sin(glm::radians(45.0f)) / 1.5f) < 1e-4f, "the ray refracts by 1 / IOR");
    Require(exit.x < 0.0f && exit.z < 0.0f, "the ray continues away from the viewer, into the surface");
    Require(std::abs(glm::length(exit) - 1.0f) < 1e-5f, "the ray travels the thickness");
}

void LodFollowsTheViewer()
{
    Require(shader::TransmissionLod(0.0f, 1.5f) == 0.0f, "a smooth surface samples the sharp copy");
    Require(shader::TransmissionLod(1.0f, 1.0f) == 0.0f, "an IOR of 1 does not blur");
    Require(std::abs(shader::TransmissionLod(1.0f, 1.5f) - 10.0f) < 1e-5f, "full roughness at IOR 1.5 is log2(1024)");
    Require(std::abs(shader::TransmissionLod(0.5f, 1.25f) - 2.5f) < 1e-5f, "roughness scales by clamp(2 IOR - 2)");
}

void AttenuationIsBeerLambert()
{
    const glm::vec3 white(1.0f);
    const glm::vec3 color(0.5f, 0.25f, 1.0f);
    Require(Near(shader::ApplyVolumeAttenuation(white, 1.0f, color, 1.0f), color), "at the attenuation distance white turns the colour");
    Require(Near(shader::ApplyVolumeAttenuation(white, 2.0f, color, 1.0f), color * color), "twice as far, squared");
    Require(Near(shader::ApplyVolumeAttenuation(white, 0.0f, color, 1.0f), white), "no distance, no absorption");
    Require(Near(shader::ApplyVolumeAttenuation(white, 5.0f, color, 0.0f), white), "attenuation distance 0 means none");
}
}

int main()
{
    try
    {
        ThinWallsDoNotRefract();
        VolumesRefractThroughTheirThickness();
        LodFollowsTheViewer();
        AttenuationIsBeerLambert();
    }
    catch (const std::exception& error)
    {
        std::cerr << "transmission tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "transmission tests passed\n";
    return 0;
}
