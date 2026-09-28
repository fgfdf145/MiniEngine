#include <engine/renderer/atmosphere.h>
#include <engine/renderer/spherical_harmonics.h>

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
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f, -10.0f, 0.0f), nullptr);
    Require(data.sunDirectionAndMode.w == 1.0f, "the mode is carried in w");
    Require(glm::length(glm::vec3(data.sunDirectionAndMode) - sun.directionToSun) < 1e-6f, "the sun direction is carried");
    Require(glm::vec3(data.sunIlluminance) == sun.illuminance, "the sun illuminance is carried");
    Require(std::fabs(data.sunIlluminance.w - std::cos(glm::radians(0.545f * 0.5f))) < 1e-7f, "w is cos of the sun's angular radius");
    const float altitude = glm::length(glm::vec3(data.cameraPositionKm)) - p.bottomRadiusKm;
    Require(altitude >= 0.0f && altitude < 0.002f, "a camera below the ground is lifted to it, got " + std::to_string(altitude));
    Require(data.hdriParameters.x == 2000.0f && data.hdriParameters.y == 0.25f, "HDRI intensity and rotation in turns");

    const EnvironmentUniformData high =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f, 200000.0f, 0.0f), nullptr);
    const float highAltitude = glm::length(glm::vec3(high.cameraPositionKm)) - p.bottomRadiusKm;
    Require(std::fabs(highAltitude - 99.0f) < 0.01f, "a camera above the atmosphere is held 1 km below its top");

    const EnvironmentUniformData dark =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, std::nullopt, glm::vec3(0.0f), nullptr);
    Require(glm::vec3(dark.sunIlluminance) == glm::vec3(0.0f), "no sun, no illuminance");

    Require(data.groundAlbedo.w == 0.0f, "no ground plane unless the settings ask for one");
    SceneEnvironment withGround = environment;
    withGround.atmosphere.groundPlane = true;
    Require(BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, withGround, p, sun, glm::vec3(0.0f), nullptr).groundAlbedo.w == 1.0f,
            "the ground plane flag is carried in groundAlbedo.w");
    Require(BuildEnvironmentUniformData(EnvironmentMode::Hdri, withGround, p, sun, glm::vec3(0.0f), nullptr).groundAlbedo.w == 0.0f,
            "the ground plane belongs to the atmosphere and is off under an HDRI");

    ShCoefficients sh{};
    sh[0] = glm::vec3(1.0f, 2.0f, 3.0f);
    sh[3] = glm::vec3(0.5f);
    SceneEnvironment hdri = environment;
    hdri.mode = EnvironmentMode::Hdri;
    hdri.hdri.intensity = 10.0f;
    hdri.hdri.rotationDegrees = 90.0f;
    const EnvironmentUniformData withSh =
        BuildEnvironmentUniformData(EnvironmentMode::Hdri, hdri, p, std::nullopt, glm::vec3(0.0f), &sh);
    const ShCoefficients expected = ShForHdriRotation(sh, 90.0f);
    for (int index = 0; index < 9; ++index)
    {
        Require(glm::length(glm::vec3(withSh.hdriIrradianceSh[index]) - expected[index] * 10.0f) < 1e-5f,
                "the HDRI SH is rotated and scaled by the intensity");
    }
}

void PacksHeightFog()
{
    SceneEnvironment environment{};
    environment.mode = EnvironmentMode::Atmosphere;
    const AtmosphereParameters p = BuildAtmosphereParameters(environment.atmosphere);
    HeightFogSettings& fog = environment.heightFog;
    fog.enabled = true;
    fog.density = 0.004f;
    fog.heightFalloff = 0.05f;
    fog.fogHeight = 3.0f;
    fog.startDistance = 20.0f;
    fog.maxOpacity = 0.8f;
    fog.albedo = glm::vec3(0.9f, 0.8f, 0.7f);
    fog.anisotropy = 0.5f;

    const EnvironmentUniformData data =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, std::nullopt, glm::vec3(0.0f), nullptr);
    Require(data.heightFogDensity == glm::vec4(0.004f, 0.05f, 3.0f, 20.0f), "density, falloff, height and start are packed");
    Require(data.heightFogColor == glm::vec4(0.9f, 0.8f, 0.7f, 0.8f), "albedo and max opacity are packed");
    Require(data.heightFogParams.x == 0.5f, "anisotropy is packed");
    Require(glm::vec3(data.heightFogParams.y, data.heightFogParams.z, data.heightFogParams.w) == glm::vec3(0.0f), "no sun, no sun glow");

    AtmosphereSun sun{};
    sun.directionToSun = glm::vec3(0.0f, 1.0f, 0.0f);
    sun.illuminance = glm::vec3(100000.0f);
    const EnvironmentUniformData lit =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f), nullptr);
    const glm::vec3 expectedSun = sun.illuminance * ComputeTransmittanceToSpace(p, 0.0005f, 1.0f) * fog.albedo;
    Require(glm::length(glm::vec3(lit.heightFogParams.y, lit.heightFogParams.z, lit.heightFogParams.w) - expectedSun) < 1.0f,
            "the sun at the camera is carried, dimmed by the air and tinted");
    sun.directionToSun = glm::normalize(glm::vec3(1.0f, -0.2f, 0.0f));
    const EnvironmentUniformData set =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f), nullptr);
    Require(set.heightFogParams.y == 0.0f && set.heightFogParams.w == 0.0f, "a set sun adds no glow");

    SceneEnvironment off = environment;
    off.heightFog.enabled = false;
    Require(BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, off, p, std::nullopt, glm::vec3(0.0f), nullptr).heightFogDensity.x == 0.0f,
            "a disabled fog uploads density 0");
    Require(BuildEnvironmentUniformData(EnvironmentMode::Hdri, environment, p, std::nullopt, glm::vec3(0.0f), nullptr).heightFogDensity.x == 0.0f,
            "the fog is off outside the Atmosphere mode");

    SceneEnvironment wild = environment;
    wild.heightFog.density = 5.0f;
    wild.heightFog.heightFalloff = 0.0f;
    wild.heightFog.startDistance = -10.0f;
    wild.heightFog.maxOpacity = 2.0f;
    wild.heightFog.albedo = glm::vec3(-1.0f, 2.0f, 0.5f);
    wild.heightFog.anisotropy = 1.0f;
    const EnvironmentUniformData clamped =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, wild, p, std::nullopt, glm::vec3(0.0f), nullptr);
    Require(clamped.heightFogDensity == glm::vec4(1.0f, 1e-5f, 3.0f, 0.0f), "density, falloff and start are clamped");
    Require(clamped.heightFogColor == glm::vec4(0.0f, 1.0f, 0.5f, 1.0f), "albedo and max opacity are clamped");
    Require(clamped.heightFogParams.x == 0.95f, "anisotropy is clamped");
}

void PacksClouds()
{
    SceneEnvironment environment{};
    environment.mode = EnvironmentMode::Atmosphere;
    const AtmosphereParameters p = BuildAtmosphereParameters(environment.atmosphere);
    CloudSettings& clouds = environment.clouds;
    clouds.enabled = true;
    clouds.coverage = 0.5f;
    clouds.baseAltitude = 2000.0f;
    clouds.thickness = 1000.0f;
    clouds.density = 0.05f;
    clouds.shapeScale = 4000.0f;
    clouds.detailScale = 500.0f;
    clouds.weatherScale = 20000.0f;
    clouds.detailErosion = 0.25f;
    clouds.forwardAnisotropy = 0.75f;
    clouds.backAnisotropy = -0.25f;
    clouds.backWeight = 0.5f;
    clouds.albedo = 0.875f;
    clouds.ambientScale = 2.0f;
    clouds.hazeDistance = 50000.0f;

    const EnvironmentUniformData data =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, std::nullopt, glm::vec3(0.0f), nullptr);
    Require(glm::length(data.cloudLayer - glm::vec4(2.0f, 1.0f, 0.5f, 50.0f)) < 1e-4f, "the layer is packed in km, extinction per km");
    Require(data.cloudScales == glm::vec4(0.25f, 2.0f, 0.05f, 0.25f), "the tile sizes become frequencies per km");
    Require(data.cloudPhase == glm::vec4(0.75f, -0.25f, 0.5f, 0.875f), "the lobes and albedo are packed");
    Require(glm::length(data.cloudParams - glm::vec4(2.0f, 50.0f, 0.0f, 0.0f)) < 1e-4f,
            "ambient and haze are packed; the frame index is the renderer's");

    SceneEnvironment off = environment;
    off.clouds.enabled = false;
    Require(BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, off, p, std::nullopt, glm::vec3(0.0f), nullptr).cloudLayer.w == 0.0f,
            "disabled clouds upload no extinction");
    Require(BuildEnvironmentUniformData(EnvironmentMode::Hdri, environment, p, std::nullopt, glm::vec3(0.0f), nullptr).cloudLayer.w == 0.0f,
            "the clouds are off outside the Atmosphere mode");

    SceneEnvironment wild = environment;
    wild.clouds.coverage = 3.0f;
    wild.clouds.density = 10.0f;
    const EnvironmentUniformData clamped =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, wild, p, std::nullopt, glm::vec3(0.0f), nullptr);
    Require(clamped.cloudLayer.z == 1.0f && clamped.cloudLayer.w == 500.0f, "coverage and density are clamped");
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
        PacksHeightFog();
        PacksClouds();
    }
    catch (const std::exception& error)
    {
        std::cerr << "atmosphere tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "atmosphere tests passed\n";
    return 0;
}
