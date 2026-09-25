#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// The anisotropic lobe and G-buffer encoding the shaders use, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/anisotropy_common.glsl>
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

// The integral of D(h) (n.h) over the hemisphere, in the frame n = +Z, t = +X, b = +Y.
double IntegrateProjectedD(float alphaT, float alphaB)
{
    const int thetaSteps = 2000;
    const int phiSteps = 400;
    double sum = 0.0;
    for (int i = 0; i < thetaSteps; ++i)
    {
        const double theta = (i + 0.5) * (kPi * 0.5) / thetaSteps;
        for (int j = 0; j < phiSteps; ++j)
        {
            const double phi = (j + 0.5) * (2.0 * kPi) / phiSteps;
            const glm::vec3 h(
                static_cast<float>(std::sin(theta) * std::cos(phi)),
                static_cast<float>(std::sin(theta) * std::sin(phi)),
                static_cast<float>(std::cos(theta)));
            const double d = shader::DistributionGgxAnisotropic(h.z, h.x, h.y, alphaT, alphaB);
            sum += d * h.z * std::sin(theta);
        }
    }
    return sum * (kPi * 0.5 / thetaSteps) * (2.0 * kPi / phiSteps);
}

void DistributionIsNormalised()
{
    for (const auto& alphas : {glm::vec2(0.2f, 0.2f), glm::vec2(0.5f, 0.2f), glm::vec2(1.0f, 0.3f), glm::vec2(0.3f, 0.8f)})
    {
        const double integral = IntegrateProjectedD(alphas.x, alphas.y);
        Require(std::abs(integral - 1.0) < 0.01,
                "D (n.h) integrates to 1 at alpha " + std::to_string(alphas.x) + ", " + std::to_string(alphas.y) + ": " +
                    std::to_string(integral));
    }
}

void DistributionMatchesIsotropicGgx()
{
    std::mt19937 random(3);
    for (int sample = 0; sample < 1000; ++sample)
    {
        glm::vec3 h = RandomUnitVector(random);
        h.z = std::abs(h.z);
        const float alpha = 0.05f + 0.95f * static_cast<float>(sample) / 1000.0f;
        const float a2 = alpha * alpha;
        const float denominator = h.z * h.z * (a2 - 1.0f) + 1.0f;
        const float isotropic = a2 / (kPi * denominator * denominator);
        const float anisotropic = shader::DistributionGgxAnisotropic(h.z, h.x, h.y, alpha, alpha);
        Require(std::abs(anisotropic - isotropic) <= 1e-4f * std::max(1.0f, isotropic), "equal alphas are the isotropic GGX");
    }
}

void VisibilityMatchesIsotropicHeightCorrelated()
{
    // With equal alphas the anisotropic visibility is the isotropic height-correlated Smith one.
    const float alpha = 0.4f;
    for (float NdL = 0.1f; NdL <= 1.0f; NdL += 0.3f)
    {
        for (float NdV = 0.1f; NdV <= 1.0f; NdV += 0.3f)
        {
            const float a2 = alpha * alpha;
            const float ggxV = NdL * std::sqrt(NdV * NdV * (1.0f - a2) + a2);
            const float ggxL = NdV * std::sqrt(NdL * NdL * (1.0f - a2) + a2);
            const float expected = 0.5f / (ggxV + ggxL);
            // Put V and L in the tangent plane's x direction, any azimuth works for equal alphas.
            const float TdV = std::sqrt(1.0f - NdV * NdV);
            const float TdL = std::sqrt(1.0f - NdL * NdL);
            const float actual = shader::VisibilityGgxAnisotropic(NdL, NdV, TdV, 0.0f, TdL, 0.0f, alpha, alpha);
            Require(std::abs(actual - expected) < 1e-4f * expected, "equal alphas are the height-correlated Smith visibility");
        }
    }
}

void FrameIsOrthonormal()
{
    std::mt19937 random(5);
    for (int sample = 0; sample < 10000; ++sample)
    {
        const glm::vec3 n = RandomUnitVector(random);
        const glm::vec3 t = shader::OrthonormalTangent(n);
        const glm::vec3 b = shader::OrthonormalBitangent(n);
        Require(std::abs(glm::length(t) - 1.0f) < 1e-4f && std::abs(glm::length(b) - 1.0f) < 1e-4f, "unit axes");
        Require(std::abs(glm::dot(t, n)) < 1e-4f && std::abs(glm::dot(b, n)) < 1e-4f && std::abs(glm::dot(t, b)) < 1e-4f,
                "orthogonal axes");
    }
    // Both hemispheres' poles, where the construction switches branch.
    for (const glm::vec3& n : {glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(1, 0, 0)})
    {
        Require(std::abs(glm::dot(shader::OrthonormalTangent(n), n)) < 1e-5f, "the poles have a frame too");
    }
}

void AngleEncodingRoundTrips()
{
    std::mt19937 random(9);
    const float step = kPi / 255.0f;
    for (int sample = 0; sample < 20000; ++sample)
    {
        glm::vec3 n = RandomUnitVector(random);
        // Every fourth normal sits on the frame's seam, z = 0.
        if (sample % 4 == 0)
        {
            n = glm::normalize(glm::vec3(n.x, n.y, 0.0f));
        }
        const glm::vec3 t = glm::normalize(glm::cross(n, RandomUnitVector(random)));
        const float encoded = shader::EncodeAnisotropyAngle(n, t);
        Require(encoded >= 0.0f && encoded <= 1.0f, "the angle encodes into [0, 1]");
        // What an 8-bit UNORM channel gives back.
        const float stored = std::round(encoded * 255.0f) / 255.0f;
        const glm::vec3 decoded = shader::DecodeAnisotropyTangent(n, stored);
        // The lobe is symmetric under t -> -t, so only the line matters.
        const float angle = std::acos(std::min(1.0f, std::abs(glm::dot(decoded, t))));
        Require(angle <= 0.5f * step + 1e-3f, "the direction comes back within half a step: " + std::to_string(angle));
        Require(std::abs(glm::dot(decoded, n)) < 1e-4f, "the decoded tangent lies in the surface");
    }
}

void DirectionFollowsTheGltfDefinition()
{
    // No texture (0.5 green is zero y after remapping) and no rotation: the tangent itself.
    glm::vec2 d = shader::AnisotropyDirection(glm::vec2(1.0f, 0.5f), 1.0f, 0.0f);
    Require(std::abs(d.x - 1.0f) < 1e-6f && std::abs(d.y) < 1e-6f, "the default direction is the tangent");
    // A quarter turn counter-clockwise: the bitangent.
    d = shader::AnisotropyDirection(glm::vec2(1.0f, 0.5f), 0.0f, 1.0f);
    Require(std::abs(d.x) < 1e-6f && std::abs(d.y - 1.0f) < 1e-6f, "rotation turns toward the bitangent");
    // The texture's direction, rotated by 45 degrees.
    const float c = std::cos(kPi / 4.0f);
    d = shader::AnisotropyDirection(glm::vec2(0.5f, 1.0f), c, c);
    Require(std::abs(d.x + c) < 1e-5f && std::abs(d.y - c) < 1e-5f, "the texture direction is rotated too");
}

void BentNormalLeansOnlyWhenAnisotropic()
{
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    const glm::vec3 t(1.0f, 0.0f, 0.0f);
    const glm::vec3 v = glm::normalize(glm::vec3(0.3f, 0.4f, 1.0f));
    const glm::vec3 same = shader::AnisotropicBentNormal(n, v, t, 0.0f, 0.5f);
    Require(glm::length(same - n) < 1e-5f, "no anisotropy, no bend");
    const glm::vec3 bent = shader::AnisotropicBentNormal(n, v, t, 1.0f, 0.5f);
    Require(glm::length(bent - n) > 1e-3f && std::abs(glm::length(bent) - 1.0f) < 1e-5f, "anisotropy bends the normal");
    Require(glm::dot(bent, v) > 0.0f, "the bent normal still faces the viewer");
}
}

int main()
{
    try
    {
        DistributionIsNormalised();
        DistributionMatchesIsotropicGgx();
        VisibilityMatchesIsotropicHeightCorrelated();
        FrameIsOrthonormal();
        AngleEncodingRoundTrips();
        DirectionFollowsTheGltfDefinition();
        BentNormalLeansOnlyWhenAnisotropic();
    }
    catch (const std::exception& error)
    {
        std::cerr << "anisotropy tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "anisotropy tests passed\n";
    return 0;
}
