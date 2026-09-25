#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// The thin-film Fresnel the shaders use, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/iridescence_common.glsl>
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

void NoFilmIsTheBase()
{
    const glm::vec3 baseF0(0.04f, 0.5f, 0.9f);
    for (float cosTheta = 0.1f; cosTheta <= 1.0f; cosTheta += 0.1f)
    {
        const glm::vec3 film = shader::EvaluateIridescence(1.3f, cosTheta, 0.0f, baseF0);
        const glm::vec3 schlick = shader::SchlickFresnel(baseF0, cosTheta);
        Require(glm::length(film - schlick) < 0.03f,
                "a film of no thickness reflects the base, at cos " + std::to_string(cosTheta) + ": " + std::to_string(film.x) +
                    " against " + std::to_string(schlick.x));
    }
}

void StaysInRange()
{
    for (float thickness = 0.0f; thickness <= 1200.0f; thickness += 25.0f)
    {
        for (float cosTheta = 0.05f; cosTheta <= 1.0f; cosTheta += 0.05f)
        {
            for (float base : {0.02f, 0.04f, 0.2f, 0.9f})
            {
                const glm::vec3 f = shader::EvaluateIridescence(1.3f, cosTheta, thickness, glm::vec3(base));
                Require(f.x >= 0.0f && f.y >= 0.0f && f.z >= 0.0f, "reflectance is never negative");
                Require(f.x <= 1.0f && f.y <= 1.0f && f.z <= 1.0f,
                        "reflectance stays within 1, got " + std::to_string(std::max({f.x, f.y, f.z})) +
                            " at " + std::to_string(thickness) + " nm");
            }
        }
    }
}

void HueChangesWithThickness()
{
    // The hue of the reflection, as the normalised chromaticity, differs between films.
    const auto chroma = [](float thickness)
    {
        const glm::vec3 f = shader::EvaluateIridescence(1.3f, 1.0f, thickness, glm::vec3(0.04f));
        return f / std::max(f.x + f.y + f.z, 1e-6f);
    };
    const glm::vec3 a = chroma(200.0f);
    const glm::vec3 b = chroma(300.0f);
    const glm::vec3 c = chroma(400.0f);
    Require(glm::length(a - b) > 0.05f && glm::length(b - c) > 0.05f && glm::length(a - c) > 0.05f,
            "films of 200, 300 and 400 nm have different hues");
}

void TotalInternalReflection()
{
    // A film of lower index than the outside medium seen at grazing angles reflects everything.
    const glm::vec3 f = shader::EvaluateIridescence(0.5f, 0.1f, 300.0f, glm::vec3(0.04f));
    Require(glm::length(f - glm::vec3(1.0f)) < 1e-6f, "total internal reflection returns 1");
}
}

int main()
{
    try
    {
        NoFilmIsTheBase();
        StaysInRange();
        HueChangesWithThickness();
        TotalInternalReflection();
    }
    catch (const std::exception& error)
    {
        std::cerr << "iridescence tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "iridescence tests passed\n";
    return 0;
}
