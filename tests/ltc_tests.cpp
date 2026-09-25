#include <engine/renderer/ltc_table.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// The polygon integration the area lights use, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/ltc_common.glsl>
#include <shaders/vulkan/brdf_common.glsl>
}

using namespace me;

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

// The table as the shader samples it: bilinear between texel centres, clamped at the edges.
glm::vec4 SampleTable(const std::array<float, kLtcTableSize * kLtcTableSize * 4>& table, float roughness, float NdV)
{
    const float size = static_cast<float>(kLtcTableSize);
    const float u = std::clamp(roughness * size - 0.5f, 0.0f, size - 1.0f);
    const float v = std::clamp(std::sqrt(1.0f - NdV) * size - 0.5f, 0.0f, size - 1.0f);
    const int r0 = static_cast<int>(u);
    const int c0 = static_cast<int>(v);
    const int r1 = std::min(r0 + 1, static_cast<int>(kLtcTableSize) - 1);
    const int c1 = std::min(c0 + 1, static_cast<int>(kLtcTableSize) - 1);
    const float fu = u - r0;
    const float fv = v - c0;
    const auto texel = [&](int r, int c)
    {
        const size_t i = (static_cast<size_t>(r) * kLtcTableSize + c) * 4;
        return glm::vec4(table[i], table[i + 1], table[i + 2], table[i + 3]);
    };
    return glm::mix(glm::mix(texel(r0, c0), texel(r0, c1), fv), glm::mix(texel(r1, c0), texel(r1, c1), fv), fu);
}

struct Quad
{
    glm::vec3 corners[4];
};

// A rectangle of the given size centred at centre, facing the receiver at the origin.
Quad MakeQuad(const glm::vec3& centre, float width, float height, float tilt)
{
    const glm::vec3 normal = glm::normalize(-centre);
    const glm::vec3 helper = std::abs(normal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 right = glm::normalize(glm::cross(helper, normal));
    glm::vec3 up = glm::cross(normal, right);
    const float c = std::cos(tilt);
    const float s = std::sin(tilt);
    const glm::vec3 r2 = c * right + s * up;
    const glm::vec3 u2 = -s * right + c * up;
    Quad quad;
    quad.corners[0] = centre - r2 * (0.5f * width) - u2 * (0.5f * height);
    quad.corners[1] = centre + r2 * (0.5f * width) - u2 * (0.5f * height);
    quad.corners[2] = centre + r2 * (0.5f * width) + u2 * (0.5f * height);
    quad.corners[3] = centre - r2 * (0.5f * width) + u2 * (0.5f * height);
    return quad;
}

// Integrates brdfCos(L) over the quad by a dense grid over its area.
template <typename F>
double IntegrateOverQuad(const Quad& quad, F brdfCos)
{
    const int n = 400;
    const glm::vec3 e1 = quad.corners[1] - quad.corners[0];
    const glm::vec3 e2 = quad.corners[3] - quad.corners[0];
    const glm::vec3 areaNormal = glm::cross(e1, e2);
    const float area = glm::length(areaNormal);
    const glm::vec3 lightNormal = areaNormal / area;
    double sum = 0.0;
    for (int j = 0; j < n; ++j)
    {
        for (int i = 0; i < n; ++i)
        {
            const glm::vec3 p = quad.corners[0] + e1 * ((i + 0.5f) / n) + e2 * ((j + 0.5f) / n);
            const float distance2 = glm::dot(p, p);
            const glm::vec3 L = p / std::sqrt(distance2);
            const float cosLight = std::abs(glm::dot(lightNormal, L));
            sum += brdfCos(L) * cosLight / distance2;
        }
    }
    return sum * area / (static_cast<double>(n) * n);
}

float LtcIntegral(const glm::mat3& minv, const Quad& quad)
{
    return shader::LtcIntegrateQuad(minv * quad.corners[0], minv * quad.corners[1], minv * quad.corners[2], minv * quad.corners[3]);
}

void IdentityIsTheClippedFormFactor()
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    std::mt19937 random(21);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    for (int sample = 0; sample < 40; ++sample)
    {
        // Some quads straddle the horizon, which the clipping must handle.
        const glm::vec3 centre(uniform(random) * 2.0f, uniform(random) * 2.0f, uniform(random) * 1.0f + 0.6f);
        const Quad quad = MakeQuad(centre, 0.5f + std::abs(uniform(random)), 0.5f + std::abs(uniform(random)), uniform(random));
        const double reference = IntegrateOverQuad(quad, [&](const glm::vec3& L)
                                                   {
                                                       return std::max(0.0f, L.z) / kPi;
                                                   });
        const float ltc = LtcIntegral(glm::mat3(1.0f), quad);
        Require(std::abs(ltc - reference) < 0.01 * std::max(reference, 0.01),
                "the cosine's integral is the clipped form factor: " + std::to_string(ltc) + " against " + std::to_string(reference));
    }
}

void FitMatchesTheLobe()
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    std::mt19937 random(23);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    double totalRelativeError = 0.0;
    double worstRelativeError = 0.0;
    double worstAwayFromGrazing = 0.0;
    int count = 0;
    for (float roughness : {0.3f, 0.5f, 0.75f, 1.0f})
    {
        const float alpha = roughness * roughness;
        for (float NdV : {0.2f, 0.5f, 0.8f, 0.98f})
        {
            const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
            const glm::mat3 minv = shader::LtcInverseMatrix(SampleTable(kLtcInverseMatrices, roughness, NdV), N, V);
            const float norm = SampleTable(kLtcAmplitudes, roughness, NdV).x;
            const glm::vec3 R = glm::reflect(-V, N);
            for (int sample = 0; sample < 6; ++sample)
            {
                // Lights around the reflection direction, where the lobe carries its energy.
                const glm::vec3 direction = glm::normalize(R + glm::vec3(uniform(random), uniform(random), std::abs(uniform(random))) * 0.6f);
                const Quad quad = MakeQuad(direction * 2.0f, 0.8f, 0.6f, uniform(random));
                const double reference = IntegrateOverQuad(
                    quad,
                    [&](const glm::vec3& L)
                    {
                        if (L.z <= 0.0f)
                        {
                            return 0.0f;
                        }
                        const glm::vec3 H = glm::normalize(V + L);
                        const float a2 = alpha * alpha;
                        const float d = H.z * H.z * (a2 - 1.0f) + 1.0f;
                        return a2 / (kPi * d * d) * shader::VisibilitySmithGgxCorrelated(NdV, L.z, alpha) * L.z;
                    });
                if (reference < 0.02 * norm)
                {
                    continue; // too little of the lobe to measure a relative error on
                }
                const double ltc = norm * LtcIntegral(minv, quad);
                const double relative = std::abs(ltc - reference) / reference;
                totalRelativeError += relative;
                worstRelativeError = std::max(worstRelativeError, relative);
                if (NdV >= 0.8f)
                {
                    worstAwayFromGrazing = std::max(worstAwayFromGrazing, relative);
                }
                ++count;
            }
        }
    }
    const double mean = totalRelativeError / count;
    std::cout << "LTC fit over " << count << " lights: mean relative error " << mean << ", worst " << worstRelativeError
              << ", worst at N.V >= 0.8 " << worstAwayFromGrazing << '\n';
    // An LTC is a fit: it is weakest at grazing views for lights that cover a few percent of the
    // lobe (up to about half the value there, as Heitz et al. report for theirs), and close
    // elsewhere.
    Require(count > 50, "enough lights were measured");
    Require(mean < 0.1, "the fit follows the lobe on average");
    Require(worstRelativeError < 0.6, "grazing, small lights stay within the fit's known error");
    Require(worstAwayFromGrazing < 0.15, "away from grazing the fit is close");
}

void TableIsFinite()
{
    for (float value : kLtcInverseMatrices)
    {
        Require(std::isfinite(value), "the matrix table is finite");
    }
    for (size_t i = 0; i < kLtcAmplitudes.size(); i += 4)
    {
        Require(kLtcAmplitudes[i] > 0.0f && kLtcAmplitudes[i] <= 1.001f, "the albedo lies in (0, 1]");
        Require(kLtcAmplitudes[i + 1] >= 0.0f && kLtcAmplitudes[i + 1] <= kLtcAmplitudes[i], "the Fresnel share lies within it");
    }
}
}

int main()
{
    try
    {
        TableIsFinite();
        IdentityIsTheClippedFormFactor();
        FitMatchesTheLobe();
    }
    catch (const std::exception& error)
    {
        std::cerr << "LTC tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "LTC tests passed\n";
    return 0;
}
