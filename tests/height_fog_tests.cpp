#include <engine/renderer/height_fog.h>

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

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

HeightFogSettings MakeFog()
{
    HeightFogSettings fog{};
    fog.enabled = true;
    fog.density = 0.002f;
    fog.heightFalloff = 0.02f;
    fog.fogHeight = 5.0f;
    fog.startDistance = 10.0f;
    return fog;
}

// Midpoint rule over the extinction, in double.
double NumericalOpticalDepth(const HeightFogSettings& fog, const glm::vec3& camera, const glm::vec3& direction, double distance)
{
    constexpr int kSteps = 10000;
    const double start = fog.startDistance;
    if (distance <= start)
    {
        return 0.0;
    }
    const double dt = (distance - start) / kSteps;
    double depth = 0.0;
    for (int step = 0; step < kSteps; ++step)
    {
        const double t = start + (step + 0.5) * dt;
        const double y = camera.y + t * direction.y;
        depth += fog.density * std::exp(-(y - fog.fogHeight) * fog.heightFalloff) * dt;
    }
    return depth;
}

void ClosedFormMatchesIntegral()
{
    const HeightFogSettings fog = MakeFog();
    const glm::vec3 camera(3.0f, 20.0f, -7.0f);
    const glm::vec3 directions[] = {
        glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f)),
        glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)),
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::normalize(glm::vec3(1.0f, 0.001f, 0.0f)),
        glm::normalize(glm::vec3(1.0f, -0.05f, 0.0f)),
    };
    for (const glm::vec3& direction : directions)
    {
        for (float distance : {15.0f, 100.0f, 400.0f, 3000.0f})
        {
            if (camera.y + distance * direction.y < -2000.0f)
            {
                continue;
            }
            const double expected = NumericalOpticalDepth(fog, camera, direction, distance);
            const double actual = HeightFogOpticalDepth(fog, camera, direction, distance);
            Require(std::abs(actual - expected) <= 1e-4 * expected + 1e-7,
                    "closed form " + std::to_string(actual) + " vs integral " + std::to_string(expected) + " at distance " +
                        std::to_string(distance) + ", direction y " + std::to_string(direction.y));
        }
    }
}

void NearHorizontalIsContinuous()
{
    const HeightFogSettings fog = MakeFog();
    const glm::vec3 camera(0.0f, 2.0f, 0.0f);
    const float distance = 510.0f; // L = 500, so k L = 1e-2 at d.y = 1e-3
    float previous = -1.0f;
    for (float y = -2e-3f; y <= 2e-3f; y += 1e-5f)
    {
        const glm::vec3 direction = glm::normalize(glm::vec3(1.0f, y, 0.0f));
        const float depth = HeightFogOpticalDepth(fog, camera, direction, distance);
        if (previous >= 0.0f)
        {
            Require(depth <= previous * 1.0001f, "the depth falls steadily as the ray tips up");
            Require(std::abs(depth - previous) < previous * 2e-4f, "and has no step at the series switch");
        }
        previous = depth;
    }
}

void SkyDepth()
{
    const HeightFogSettings fog = MakeFog();
    const glm::vec3 camera(0.0f, 2.0f, 0.0f);
    float previous = 0.0f;
    for (float y : {0.5f, 0.1f, 0.01f, 1e-3f, 1e-5f})
    {
        const glm::vec3 direction = glm::normalize(glm::vec3(std::sqrt(1.0f - y * y), y, 0.0f));
        const float depth = HeightFogSkyOpticalDepth(fog, camera, direction);
        Require(std::isfinite(depth) && depth > previous, "the sky depth is finite and grows toward the horizon");
        previous = depth;
    }
    Require(previous > 1000.0f, "and diverges at it");
    Require(std::isinf(HeightFogSkyOpticalDepth(fog, camera, glm::vec3(1.0f, 0.0f, 0.0f))), "level rays never leave the fog");
    Require(std::isinf(HeightFogSkyOpticalDepth(fog, camera, glm::normalize(glm::vec3(1.0f, -0.2f, 0.0f)))), "nor do falling ones");

    const glm::vec3 up = glm::normalize(glm::vec3(0.3f, 0.8f, 0.0f));
    const float far = HeightFogOpticalDepth(fog, camera, up, 20000.0f);
    const float sky = HeightFogSkyOpticalDepth(fog, camera, up);
    Require(std::abs(far - sky) < sky * 1e-4f, "a surface far up the ray meets the sky's depth");
}

void StartDistanceBeyondSurface()
{
    const HeightFogSettings fog = MakeFog();
    Require(HeightFogOpticalDepth(fog, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 5.0f) == 0.0f, "a surface before the start is clear");
    Require(HeightFogOpticalDepth(fog, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f) == 0.0f, "and at it");
}

void DeepCameraStaysFinite()
{
    const HeightFogSettings fog = MakeFog();
    const glm::vec3 camera(0.0f, -10000.0f, 0.0f);
    for (const glm::vec3& direction : {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)})
    {
        const float depth = HeightFogOpticalDepth(fog, camera, direction, 100.0f);
        Require(std::isfinite(depth) && depth > 1e20f, "a camera 10 km below the fog height sees finite, opaque fog");
    }
    Require(std::isfinite(HeightFogSkyOpticalDepth(fog, camera, glm::vec3(0.0f, 1.0f, 0.0f))), "and a finite sky depth");

    // High above it, looking down: the fog on the ground must not underflow away.
    const glm::vec3 plane(0.0f, 5005.0f, 0.0f);
    const float down = HeightFogOpticalDepth(fog, plane, glm::vec3(0.0f, -1.0f, 0.0f), 5010.0f);
    const double expected = NumericalOpticalDepth(fog, plane, glm::vec3(0.0f, -1.0f, 0.0f), 5010.0);
    Require(std::abs(down - expected) < expected * 1e-3, "the ground fog seen from 5 km up is " + std::to_string(down));
}

void HenyeyGreensteinIsNormalized()
{
    Require(std::abs(HenyeyGreenstein(0.0f, 0.3f) - static_cast<float>(1.0 / (4.0 * kPi))) < 1e-7f, "g = 0 is isotropic");
    for (float g : {0.0f, 0.3f, 0.6f, 0.9f})
    {
        constexpr int kSteps = 20000;
        double integral = 0.0;
        for (int step = 0; step < kSteps; ++step)
        {
            const double cosTheta = -1.0 + (step + 0.5) * 2.0 / kSteps;
            integral += HenyeyGreenstein(g, static_cast<float>(cosTheta)) * 2.0 * kPi * (2.0 / kSteps);
        }
        Require(std::abs(integral - 1.0) < 1e-3, "HG integrates to 1 over the sphere at g = " + std::to_string(g));
    }
    Require(HenyeyGreenstein(0.6f, 1.0f) > HenyeyGreenstein(0.6f, -1.0f), "positive g scatters forward");
}

void ZeroDensityIsClear()
{
    HeightFogSettings fog = MakeFog();
    fog.density = 0.0f;
    Require(HeightFogOpticalDepth(fog, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1000.0f) == 0.0f, "no density, no fog");
    Require(HeightFogSkyOpticalDepth(fog, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == 0.0f, "not even at the horizon");
}
}

int main()
{
    try
    {
        ClosedFormMatchesIntegral();
        NearHorizontalIsContinuous();
        SkyDepth();
        StartDistanceBeyondSurface();
        DeepCameraStaysFinite();
        HenyeyGreensteinIsNormalized();
        ZeroDensityIsClear();
    }
    catch (const std::exception& error)
    {
        std::cerr << "height fog tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "height fog tests passed\n";
    return 0;
}
