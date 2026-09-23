#include <engine/renderer/atmosphere.h>

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

std::string Text(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

// Looking straight up from the ground the optical depth has a closed form: each exponential layer
// contributes coefficient * scale height (the 100 km column holds all but e^-12.5 of it) and the
// ozone tent contributes coefficient * its 15 km area.
void ZenithTransmittanceMatchesOpticalDepth()
{
    const AtmosphereParameters p = BuildAtmosphereParameters(AtmosphereSettings{});
    const glm::vec3 opticalDepth =
        p.rayleighScattering * p.rayleighScaleHeightKm +
        glm::vec3(p.mieExtinction * p.mieScaleHeightKm) +
        p.ozoneAbsorption * kOzoneHalfWidthKm;
    const glm::vec3 expected = glm::exp(-opticalDepth);
    const glm::vec3 actual = ComputeTransmittanceToSpace(p, 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel)
    {
        Require(std::fabs(actual[channel] - expected[channel]) < 0.005f * expected[channel],
                "zenith transmittance " + Text(actual) + ", expected " + Text(expected));
    }
    Require(std::fabs(actual.r - 0.940f) < 0.005f && std::fabs(actual.g - 0.868f) < 0.005f && std::fabs(actual.b - 0.762f) < 0.005f,
            "zenith transmittance " + Text(actual) + " is not the spec's (0.940, 0.868, 0.762)");
}

void LowSunIsDimmerAndRedder()
{
    const AtmosphereParameters p = BuildAtmosphereParameters(AtmosphereSettings{});
    glm::vec3 previous = ComputeTransmittanceToSpace(p, 0.0f, 1.0f);
    for (float elevation = 85.0f; elevation >= 1.0f; elevation -= 1.0f)
    {
        const glm::vec3 current = ComputeTransmittanceToSpace(p, 0.0f, std::sin(glm::radians(elevation)));
        Require(glm::all(glm::lessThanEqual(current, previous + glm::vec3(1e-6f))),
                "transmittance rose as the sun lowered to " + std::to_string(elevation) + " degrees");
        previous = current;
    }
    const glm::vec3 low = ComputeTransmittanceToSpace(p, 0.0f, std::sin(glm::radians(2.0f)));
    Require(low.b < low.g && low.g < low.r, "a 2 degree sun must be reddened, got " + Text(low));
}

void EdgeCases()
{
    const AtmosphereParameters p = BuildAtmosphereParameters(AtmosphereSettings{});
    const float top = p.topRadiusKm - p.bottomRadiusKm;
    Require(glm::all(glm::greaterThan(ComputeTransmittanceToSpace(p, top, 1.0f), glm::vec3(0.999999f))),
            "from the top of the atmosphere looking up nothing is in the way");
    Require(ComputeTransmittanceToSpace(p, 0.0f, -0.5f) == glm::vec3(0.0f), "a ray into the ground reaches no sky");

    AtmosphereSettings empty{};
    empty.rayleighDensityScale = 0.0f;
    empty.mieDensityScale = 0.0f;
    empty.ozoneDensityScale = 0.0f;
    Require(ComputeTransmittanceToSpace(BuildAtmosphereParameters(empty), 0.0f, 0.01f) == glm::vec3(1.0f),
            "an empty atmosphere is transparent");
}

void BuildsUniformData()
{
    SceneEnvironment environment{};
    environment.mode = EnvironmentMode::Atmosphere;
    environment.hdri.rotationDegrees = 90.0f;
    environment.hdri.intensity = 2000.0f;
    const AtmosphereParameters p = BuildAtmosphereParameters(environment.atmosphere);
    AtmosphereSun sun{};
    sun.directionToSun = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    sun.illuminance = glm::vec3(100000.0f);

    const EnvironmentUniformData data =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f, -10.0f, 0.0f));
    Require(data.sunDirectionAndMode.w == 1.0f, "the mode is carried in w");
    Require(glm::length(glm::vec3(data.sunDirectionAndMode) - sun.directionToSun) < 1e-6f, "the sun direction is carried");
    Require(glm::vec3(data.sunIlluminance) == sun.illuminance, "the sun illuminance is carried");
    Require(std::fabs(data.sunIlluminance.w - std::cos(glm::radians(0.545f * 0.5f))) < 1e-7f, "w is cos of the sun's angular radius");
    const float altitude = glm::length(glm::vec3(data.cameraPositionKm)) - p.bottomRadiusKm;
    Require(altitude >= 0.0f && altitude < 0.002f, "a camera below the ground is lifted to it, got " + std::to_string(altitude));
    Require(data.hdriParameters.x == 2000.0f && data.hdriParameters.y == 0.25f, "HDRI intensity and rotation in turns");

    const EnvironmentUniformData high =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f, 200000.0f, 0.0f));
    const float highAltitude = glm::length(glm::vec3(high.cameraPositionKm)) - p.bottomRadiusKm;
    Require(std::fabs(highAltitude - 99.0f) < 0.01f, "a camera above the atmosphere is held 1 km below its top");

    const EnvironmentUniformData dark =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, std::nullopt, glm::vec3(0.0f));
    Require(glm::vec3(dark.sunIlluminance) == glm::vec3(0.0f), "no sun, no illuminance");
}
}

int main()
{
    try
    {
        ZenithTransmittanceMatchesOpticalDepth();
        LowSunIsDimmerAndRedder();
        EdgeCases();
        BuildsUniformData();
    }
    catch (const std::exception& error)
    {
        std::cerr << "atmosphere tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "atmosphere tests passed\n";
    return 0;
}
