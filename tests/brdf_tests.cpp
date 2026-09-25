#include <engine/renderer/environment_brdf.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// The BRDF terms the shaders use, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/brdf_common.glsl>
}

namespace
{
constexpr float kPi = 3.14159265f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

glm::vec3 RandomUnitVector(std::mt19937& random)
{
    std::normal_distribution<float> normal;
    glm::vec3 v(0.0f);
    while (glm::dot(v, v) < 1e-6f)
    {
        v = glm::vec3(normal(random), normal(random), normal(random));
    }
    return glm::normalize(v);
}

void CorrelatedVisibilityIsHeitzs()
{
    for (float alpha : {0.01f, 0.1f, 0.5f, 1.0f})
    {
        for (float NdV = 0.05f; NdV <= 1.0f; NdV += 0.19f)
        {
            for (float NdL = 0.05f; NdL <= 1.0f; NdL += 0.19f)
            {
                // Heitz 2014, eq. 99: G2 = 1 / (1 + Lambda(V) + Lambda(L)), V = G2 / (4 N.L N.V).
                const auto lambda = [alpha](float c)
                {
                    const float tan2 = (1.0f - c * c) / (c * c);
                    return 0.5f * (-1.0f + std::sqrt(1.0f + alpha * alpha * tan2));
                };
                const float expected = 1.0f / (1.0f + lambda(NdV) + lambda(NdL)) / (4.0f * NdL * NdV);
                const float actual = shader::VisibilitySmithGgxCorrelated(NdV, NdL, alpha);
                Require(std::abs(actual - expected) <= 1e-4f * expected, "the correlated visibility is Heitz's");
            }
        }
    }
}

// The directional albedo of Burley's diffuse for a white surface, integral f_d N.L over the
// hemisphere, by quadrature.
float BurleyAlbedo(float NdV, float roughness)
{
    const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
    const int thetaSteps = 128;
    const int phiSteps = 128;
    double sum = 0.0;
    for (int i = 0; i < thetaSteps; ++i)
    {
        const float theta = (i + 0.5f) * (kPi * 0.5f) / thetaSteps;
        for (int j = 0; j < phiSteps; ++j)
        {
            const float phi = (j + 0.5f) * (2.0f * kPi) / phiSteps;
            const glm::vec3 L(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
            const glm::vec3 H = glm::normalize(V + L);
            sum += shader::BurleyDiffuse(NdV, L.z, glm::dot(L, H), roughness) * L.z * std::sin(theta);
        }
    }
    return static_cast<float>(sum * (kPi * 0.5f / thetaSteps) * (2.0f * kPi / phiSteps));
}

void BurleyDiffuseConservesEnergy()
{
    float lowest = 10.0f;
    float highest = 0.0f;
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.25f)
    {
        for (float NdV = 0.05f; NdV <= 1.0f; NdV += 0.1f)
        {
            const float albedo = BurleyAlbedo(NdV, roughness);
            lowest = std::min(lowest, albedo);
            highest = std::max(highest, albedo);
        }
    }
    Require(highest <= 1.05f, "Burley never reflects much more than it receives: " + std::to_string(highest));
    // A smooth surface seen at grazing angles loses most of its diffuse to the two Fresnel
    // transmissions, which is what Burley models; it never goes negative.
    Require(lowest > 0.1f, "it darkens toward grazing but stays positive: " + std::to_string(lowest));
    Require(BurleyAlbedo(0.05f, 1.0f) > BurleyAlbedo(0.05f, 0.0f), "rough surfaces keep more at grazing (retroreflection)");
    Require(std::abs(BurleyAlbedo(1.0f, 0.0f) - 1.0f) < 0.05f, "smooth and head on it is Lambert");
}

// The DFG table's A + B against an independent quadrature over outgoing directions.
void TableMatchesQuadratureWithCorrelatedVisibility()
{
    // Rough enough for a regular grid over outgoing directions to resolve the lobe; the mirror
    // limit is tested in environment_brdf_tests.cpp.
    for (float roughness : {0.5f, 0.8f, 1.0f})
    {
        for (float NdV : {0.1f, 0.5f, 0.9f})
        {
            const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
            const float alpha = roughness * roughness;
            const int thetaSteps = 1024;
            const int phiSteps = 256;
            double sum = 0.0;
            for (int i = 0; i < thetaSteps; ++i)
            {
                const float theta = (i + 0.5f) * (kPi * 0.5f) / thetaSteps;
                for (int j = 0; j < phiSteps; ++j)
                {
                    const float phi = (j + 0.5f) * (2.0f * kPi) / phiSteps;
                    const glm::vec3 L(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
                    const glm::vec3 H = glm::normalize(V + L);
                    const float a2 = alpha * alpha;
                    const float d = H.z * H.z * (a2 - 1.0f) + 1.0f;
                    const float D = a2 / (kPi * d * d);
                    sum += D * shader::VisibilitySmithGgxCorrelated(NdV, L.z, alpha) * L.z * std::sin(theta);
                }
            }
            const float reference = static_cast<float>(sum * (kPi * 0.5f / thetaSteps) * (2.0f * kPi / phiSteps));
            const glm::vec2 ab = me::IntegrateEnvironmentBrdf(roughness, NdV, 4096);
            Require(ab.x + ab.y <= 1.0f + 1e-3f, "the table never reflects more than it receives");
            Require(std::abs(ab.x + ab.y - reference) < 0.01f,
                    "the table's A + B is the correlated lobe's albedo at roughness " + std::to_string(roughness) + ", N.V " +
                        std::to_string(NdV) + ": " + std::to_string(ab.x + ab.y) + " against " + std::to_string(reference));
        }
    }
}

void DiskDirectionStaysOnTheDisk()
{
    std::mt19937 random(13);
    const float radius = 0.00475f * 20.0f; // a disk large enough to test at float precision
    const float cosRadius = std::cos(radius);
    const float sinRadius = std::sin(radius);
    for (int sample = 0; sample < 5000; ++sample)
    {
        const glm::vec3 centre = RandomUnitVector(random);
        const glm::vec3 R = RandomUnitVector(random);
        const glm::vec3 L = shader::DiskLightSpecularDirection(centre, R, cosRadius, sinRadius);
        Require(std::abs(glm::length(L) - 1.0f) < 1e-4f, "a unit direction");
        if (glm::dot(centre, R) >= cosRadius)
        {
            Require(glm::length(L - R) < 1e-5f, "inside the disk the reflection itself");
        }
        else
        {
            Require(std::abs(glm::dot(L, centre) - cosRadius) < 1e-4f, "outside, on the disk's edge");
            const glm::vec3 planeNormal = glm::cross(centre, R);
            if (glm::length(planeNormal) > 1e-3f)
            {
                Require(std::abs(glm::dot(L, glm::normalize(planeNormal))) < 1e-3f, "in the plane of R and the centre");
                Require(glm::dot(L, R) >= glm::dot(centre, R) - 1e-5f, "toward R");
            }
        }
    }
}

void SphereDirectionStaysInTheSphere()
{
    std::mt19937 random(17);
    for (int sample = 0; sample < 5000; ++sample)
    {
        const glm::vec3 toLight = RandomUnitVector(random) * (0.5f + static_cast<float>(sample % 50));
        const glm::vec3 R = RandomUnitVector(random);
        const float radius = 0.01f * static_cast<float>(sample % 40);
        const glm::vec3 L = shader::SphereLightSpecularVector(toLight, R, radius);
        Require(glm::length(L - toLight) <= radius + 1e-4f, "the point lies within the sphere");
        if (radius == 0.0f)
        {
            Require(glm::length(L - toLight) < 1e-6f, "a point light is its centre");
        }
    }
    Require(std::abs(shader::SourceSizeNormalization(0.3f, 0.0f) - 1.0f) < 1e-6f, "no source, no renormalisation");
    for (float halfAngle = 0.01f; halfAngle < 2.0f; halfAngle += 0.1f)
    {
        const float n = shader::SourceSizeNormalization(0.1f, halfAngle);
        Require(n > 0.0f && n <= 1.0f, "a source only spreads the lobe");
    }
}

void HorizonFades()
{
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    Require(shader::HorizonSpecularOcclusion(glm::vec3(0.0f, 0.0f, 1.0f), n) == 1.0f, "straight up sees all");
    Require(shader::HorizonSpecularOcclusion(glm::vec3(1.0f, 0.0f, 0.0f), n) == 1.0f, "the horizon itself sees all");
    Require(shader::HorizonSpecularOcclusion(glm::vec3(0.0f, 0.0f, -1.0f), n) == 0.0f, "straight down sees nothing");
    float previous = 1.0f;
    for (float angle = 0.0f; angle <= kPi * 0.5f; angle += 0.05f)
    {
        const float h = shader::HorizonSpecularOcclusion(glm::vec3(std::cos(angle), 0.0f, -std::sin(angle)), n);
        Require(h <= previous + 1e-6f, "deeper below the horizon, less seen");
        previous = h;
    }
}
}

int main()
{
    try
    {
        CorrelatedVisibilityIsHeitzs();
        BurleyDiffuseConservesEnergy();
        TableMatchesQuadratureWithCorrelatedVisibility();
        DiskDirectionStaysOnTheDisk();
        SphereDirectionStaysInTheSphere();
        HorizonFades();
    }
    catch (const std::exception& error)
    {
        std::cerr << "BRDF tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "BRDF tests passed\n";
    return 0;
}
