#include <engine/renderer/volumetric_clouds.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr float kPlanet = 6360.0f;
constexpr float kInner = kPlanet + 1.5f;
constexpr float kOuter = kInner + 2.5f;
constexpr float kMaxDistance = 1000.0f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool Near(float a, float b, float tolerance)
{
    return std::abs(a - b) <= tolerance;
}

void ClampsSettings()
{
    CloudSettings wild{};
    wild.coverage = 2.0f;
    wild.baseAltitude = -5.0f;
    wild.thickness = 1e6f;
    wild.density = 0.0f;
    wild.shapeScale = 1.0f;
    wild.detailScale = 1e9f;
    wild.weatherScale = 0.0f;
    wild.detailErosion = -1.0f;
    wild.forwardAnisotropy = 1.0f;
    wild.backAnisotropy = 0.5f;
    wild.backWeight = 3.0f;
    wild.albedo = 1.5f;
    wild.ambientScale = -1.0f;
    wild.hazeDistance = 0.0f;
    const CloudSettings clamped = ClampCloudSettings(wild);
    Require(clamped.coverage == 1.0f && clamped.baseAltitude == 100.0f && clamped.thickness == 10000.0f, "coverage and layer clamped");
    Require(clamped.density == 0.001f, "density clamped above zero");
    Require(clamped.shapeScale == 500.0f && clamped.detailScale == 10000.0f && clamped.weatherScale == 1000.0f, "scales clamped");
    Require(clamped.detailErosion == 0.0f && clamped.forwardAnisotropy == 0.95f && clamped.backAnisotropy == 0.0f, "shape and lobes clamped");
    Require(clamped.backWeight == 1.0f && clamped.albedo == 1.0f && clamped.ambientScale == 0.0f && clamped.hazeDistance == 1000.0f,
            "weights clamped");
    Require(ClampCloudSettings(CloudSettings{}) == CloudSettings{}, "the defaults are inside the ranges");
}

void HeightGradientShape()
{
    Require(CloudHeightGradient(0.0f) == 0.0f, "nothing at the base line");
    Require(CloudHeightGradient(1.0f) == 0.0f, "nothing at the top");
    Require(CloudHeightGradient(-0.5f) == 0.0f && CloudHeightGradient(1.5f) == 0.0f, "nothing outside the layer");
    Require(CloudHeightGradient(0.2f) == 1.0f, "full between the base and the thinning tops");
    Require(CloudHeightGradient(0.05f) > 0.0f && CloudHeightGradient(0.05f) < 1.0f, "the base fills in");
    Require(CloudHeightGradient(0.5f) > CloudHeightGradient(0.8f), "the tops thin out");
}

void CoverageRamps()
{
    for (float field : {0.0f, 0.3f, 0.6f, 0.85f, 1.0f})
    {
        Require(CloudCoverageRamp(field, 0.0f) == 0.0f, "coverage 0 is a clear sky");
    }
    Require(CloudCoverageRamp(0.2f, 1.0f) == 1.0f, "coverage 1 fills all but the thinnest field");
    Require(CloudCoverageRamp(0.5f, 0.45f) == 0.0f, "below the threshold, clear");
    Require(Near(CloudCoverageRamp(0.65f, 0.45f), 0.5f, 1e-5f), "half way up the edge, half density");
    Require(CloudCoverageRamp(0.8f, 0.45f) == 1.0f, "past the edge, full density");
    float previous = -1.0f;
    for (float coverage = 0.05f; coverage <= 1.0f; coverage += 0.05f)
    {
        const float density = CloudCoverageRamp(0.6f, coverage);
        Require(density >= previous, "more coverage never removes clouds");
        previous = density;
    }
}

void FieldBlendsWeatherAndShape()
{
    Require(CloudWeather(0.0f, 0.0f) == 0.0f && CloudWeather(1.0f, 1.0f) == 1.0f, "the weather spans [0, 1]");
    Require(Near(CloudWeather(0.5f, 0.5f), 0.5f, 1e-6f), "and is centred");
    Require(CloudShape(glm::vec4(1.0f)) == 1.0f, "a lobe's core is full");
    Require(CloudShape(glm::vec4(0.3f, 0.0f, 0.0f, 0.0f)) == 0.0f, "where the Worley octaves are low, the edges erode away");
    Require(CloudShape(glm::vec4(0.3f, 1.0f, 1.0f, 1.0f)) == 0.3f, "where they are high, the lobe keeps its value");
    Require(CloudField(1.0f, 1.0f, 1.0f) == 1.0f && CloudField(1.0f, 1.0f, 0.0f) == 0.0f, "the profile scales the field");
    Require(CloudField(1.0f, 0.0f, 1.0f) > CloudField(0.0f, 1.0f, 1.0f), "the weather decides where clouds gather");
}

void PhaseIsNormalized()
{
    for (float backWeight : {0.0f, 0.3f, 1.0f})
    {
        constexpr int kSteps = 20000;
        double integral = 0.0;
        for (int step = 0; step < kSteps; ++step)
        {
            const double cosTheta = -1.0 + (step + 0.5) * 2.0 / kSteps;
            integral += CloudPhase(0.8f, -0.3f, backWeight, static_cast<float>(cosTheta)) * 2.0 * kPi * (2.0 / kSteps);
        }
        Require(std::abs(integral - 1.0) < 2e-3, "the dual lobe integrates to 1 at back weight " + std::to_string(backWeight));
    }
    Require(CloudPhase(0.8f, -0.3f, 0.3f, 1.0f) > CloudPhase(0.8f, -0.3f, 0.3f, -1.0f), "silver lining: brightest toward the sun");
}

void SunScatteringOctaves()
{
    const float single = CloudPhase(0.8f, -0.3f, 0.3f, 0.2f);
    const float clear = CloudSunScattering(0.0f, 0.8f, -0.3f, 0.3f, 0.2f);
    Require(clear > single, "the octaves add light on top of single scattering");
    float octaveSum = 0.0f;
    for (int octave = 0; octave < kCloudScatteringOctaves; ++octave)
    {
        octaveSum += std::pow(kCloudOctaveScattering, static_cast<float>(octave));
    }
    Require(Near(CloudSunScattering(0.0f, 0.0f, 0.0f, 0.0f, 0.2f), octaveSum / static_cast<float>(4.0 * kPi), 1e-6f),
            "isotropic octaves sum to the geometric series of a");
    Require(kCloudOctaveScattering <= kCloudOctaveExtinction, "a <= b keeps the octaves energy conserving");
    float previous = clear;
    for (float depth : {0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 40.0f})
    {
        const float scattering = CloudSunScattering(depth, 0.8f, -0.3f, 0.3f, 0.2f);
        Require(scattering < previous, "deeper inside, less sun");
        previous = scattering;
    }
    // The last octaves see a fraction of the depth, so the core keeps light that single scattering
    // alone would lose: the grey, not black, underside.
    Require(CloudSunScattering(10.0f, 0.8f, -0.3f, 0.3f, 0.2f) > 100.0f * single * std::exp(-10.0f), "multiple scattering lights the core");
}

void ShellFromTheGround()
{
    const glm::vec3 camera(0.0f, kPlanet + 0.002f, 0.0f);
    const glm::vec2 up = CloudShellInterval(camera, glm::vec3(0.0f, 1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(Near(up.x, 1.498f, 1e-3f) && Near(up.y, 3.998f, 1e-3f), "straight up crosses the layer from its base to its top");

    const glm::vec2 down = CloudShellInterval(camera, glm::vec3(0.0f, -1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(down.y <= down.x, "looking down meets the ground first");

    const glm::vec2 level = CloudShellInterval(camera, glm::vec3(1.0f, 0.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(level.y > level.x && level.x > 100.0f, "a level ray reaches the layer beyond the horizon");
    Require(Near(level.x, std::sqrt(kInner * kInner - camera.y * camera.y), 0.05f), "where the level ray rises through the base");

    const glm::vec2 clipped = CloudShellInterval(camera, glm::vec3(1.0f, 0.0f, 0.0f), kPlanet, kInner, kOuter, 150.0f);
    Require(clipped.x == level.x && clipped.y == 150.0f, "the march stops at the maximum distance");
    const glm::vec2 beyond = CloudShellInterval(camera, glm::vec3(1.0f, 0.0f, 0.0f), kPlanet, kInner, kOuter, 50.0f);
    Require(beyond.y <= beyond.x, "nothing when the layer starts beyond the maximum distance");
}

void ShellFromInsideAndAbove()
{
    const glm::vec3 inside(0.0f, kInner + 1.0f, 0.0f);
    const glm::vec2 insideUp = CloudShellInterval(inside, glm::vec3(0.0f, 1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(insideUp.x == 0.0f && Near(insideUp.y, 1.5f, 1e-3f), "inside, the march starts at the camera");
    const glm::vec2 insideDown = CloudShellInterval(inside, glm::vec3(0.0f, -1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(insideDown.x == 0.0f && Near(insideDown.y, 1.0f, 1e-3f), "inside looking down, it ends at the base");

    const glm::vec3 above(0.0f, kOuter + 2.0f, 0.0f);
    const glm::vec2 aboveDown = CloudShellInterval(above, glm::vec3(0.0f, -1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(Near(aboveDown.x, 2.0f, 1e-3f) && Near(aboveDown.y, 4.5f, 1e-3f), "above, the layer runs from its top to its base");
    const glm::vec2 aboveUp = CloudShellInterval(above, glm::vec3(0.0f, 1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(aboveUp.y <= aboveUp.x, "above looking up sees no layer");
}
}

int main()
{
    try
    {
        ClampsSettings();
        HeightGradientShape();
        CoverageRamps();
        FieldBlendsWeatherAndShape();
        PhaseIsNormalized();
        SunScatteringOctaves();
        ShellFromTheGround();
        ShellFromInsideAndAbove();
    }
    catch (const std::exception& error)
    {
        std::cerr << "volumetric clouds tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "volumetric clouds tests passed\n";
    return 0;
}
