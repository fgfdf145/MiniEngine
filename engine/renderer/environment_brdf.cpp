#include "environment_brdf.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;

float RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

// A GGX half vector in tangent space (N = +Z).
glm::vec3 ImportanceSampleGgx(float u, float v, float roughness)
{
    const float alpha = roughness * roughness;
    const float phi = 2.0f * kPi * u;
    const float cosTheta = std::sqrt((1.0f - v) / (1.0f + (alpha * alpha - 1.0f) * v));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return glm::vec3(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
}

// The height-correlated Smith visibility, G / (4 N.L N.V), as VisibilitySmithGgxCorrelated in
// shaders/vulkan/brdf_common.glsl: the direct lights use the same term, so the table's energy
// compensation is that of the lobe they draw.
float VisibilitySmithGgxCorrelated(float NdV, float NdL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdL * std::sqrt(NdV * NdV * (1.0f - a2) + a2);
    const float ggxL = NdV * std::sqrt(NdL * NdL * (1.0f - a2) + a2);
    return 0.5f / std::max(ggxV + ggxL, 1e-7f);
}
}

glm::vec2 IntegrateEnvironmentBrdf(float roughness, float NdV, uint32_t sampleCount)
{
    NdV = std::clamp(NdV, 1e-4f, 1.0f);
    const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
    const float alpha = roughness * roughness;
    float a = 0.0f;
    float b = 0.0f;
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        const glm::vec3 H = ImportanceSampleGgx(static_cast<float>(i) / static_cast<float>(sampleCount), RadicalInverse(i), roughness);
        const glm::vec3 L = 2.0f * glm::dot(V, H) * H - V;
        const float NdL = std::clamp(L.z, 0.0f, 1.0f);
        const float NdH = std::clamp(H.z, 0.0f, 1.0f);
        const float VdH = std::clamp(glm::dot(V, H), 0.0f, 1.0f);
        if (NdL > 0.0f && NdH > 0.0f)
        {
            // pdf = D NdH / (4 VdH); dividing D Vis NdL by it leaves 4 Vis NdL VdH / NdH.
            const float visibility = 4.0f * VisibilitySmithGgxCorrelated(NdV, NdL, alpha) * NdL * VdH / NdH;
            const float fresnel = std::pow(1.0f - VdH, 5.0f);
            a += (1.0f - fresnel) * visibility;
            b += fresnel * visibility;
        }
    }
    return glm::vec2(a, b) / static_cast<float>(sampleCount);
}

glm::vec3 SpecularEnergyCompensation(const glm::vec3& f0, const glm::vec2& environmentBrdf)
{
    const float singleScatterAlbedo = std::max(environmentBrdf.x + environmentBrdf.y, 1e-4f);
    return glm::vec3(1.0f) + f0 * (1.0f / singleScatterAlbedo - 1.0f);
}

float IntegrateSheenAlbedo(float roughness, float NdV, uint32_t sampleCount)
{
    NdV = std::clamp(NdV, 1e-4f, 1.0f);
    const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
    // The same floor the shader puts on the sheen roughness, and the same guard Filament keeps
    // on sin^2 so the Charlie term stays finite at grazing half vectors.
    const float alpha = std::max(roughness, 0.04f) * std::max(roughness, 0.04f);
    double sum = 0.0;
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        // Uniform over the hemisphere: cos(theta) uniform in [0, 1], pdf = 1 / (2 pi) per steradian.
        const float cosTheta = (static_cast<float>(i) + 0.5f) / static_cast<float>(sampleCount);
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = 2.0f * kPi * RadicalInverse(i);
        const glm::vec3 H(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
        const float VdH = glm::dot(V, H);
        if (VdH <= 0.0f)
        {
            continue;
        }
        const glm::vec3 L = 2.0f * VdH * H - V;
        const float NdL = L.z;
        if (NdL <= 0.0f)
        {
            continue;
        }
        const float sin2h = std::max(1.0f - cosTheta * cosTheta, 0.0078125f);
        const float D = (2.0f + 1.0f / alpha) * std::pow(sin2h, 0.5f / alpha) / (2.0f * kPi);
        const float visibility = 1.0f / (4.0f * (NdL + NdV - NdL * NdV));
        // pdf over L is pdf over H / (4 V.H), so each sample weighs D V N.L * 4 V.H * 2 pi.
        sum += static_cast<double>(D * visibility * NdL * VdH);
    }
    return static_cast<float>(sum * 8.0 * static_cast<double>(kPi) / static_cast<double>(sampleCount));
}

FloatTextureData BuildEnvironmentBrdfLut(uint32_t size, uint32_t sampleCount)
{
    FloatTextureData table{};
    table.width = static_cast<int>(size);
    table.height = static_cast<int>(size);
    table.pixels.resize(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
    {
        const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (uint32_t x = 0; x < size; ++x)
        {
            const float NdV = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            glm::vec2 ab = IntegrateEnvironmentBrdf(roughness, NdV, sampleCount);
            // The true A + B never exceeds 1, but the correlated visibility's estimate can overshoot
            // by its noise, and the energy compensation 1 / (A + B) must then not drop below 1.
            const float albedo = ab.x + ab.y;
            if (albedo > 1.0f)
            {
                ab /= albedo;
            }
            float* texel = &table.pixels[(static_cast<size_t>(y) * size + x) * 4];
            texel[0] = ab.x;
            texel[1] = ab.y;
            // Charlie with Neubelt's visibility is not energy conserving: smooth sheen seen at
            // grazing angles integrates past 1 (1.74 at roughness 0.1, N.V 0.05). The shader
            // scales the base by 1 - max(sheenColor) * this, which must not go negative, and a
            // lobe cannot reflect more than arrives, so the table stores it clamped.
            texel[2] = std::min(IntegrateSheenAlbedo(roughness, NdV, sampleCount), 1.0f);
            texel[3] = 1.0f;
        }
    }
    return table;
}
}
