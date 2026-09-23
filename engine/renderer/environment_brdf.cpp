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

float SchlickSmithG1(float NdX, float k)
{
    return NdX / (NdX * (1.0f - k) + k);
}
}

glm::vec2 IntegrateEnvironmentBrdf(float roughness, float NdV, uint32_t sampleCount)
{
    NdV = std::clamp(NdV, 1e-4f, 1.0f);
    const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
    const float k = roughness * roughness / 2.0f;
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
            const float G = SchlickSmithG1(NdV, k) * SchlickSmithG1(NdL, k);
            // pdf = D NdH / (4 VdH); dividing the BRDF times NdL by it leaves G VdH / (NdH NdV).
            const float visibility = G * VdH / (NdH * NdV);
            const float fresnel = std::pow(1.0f - VdH, 5.0f);
            a += (1.0f - fresnel) * visibility;
            b += fresnel * visibility;
        }
    }
    return glm::vec2(a, b) / static_cast<float>(sampleCount);
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
            const glm::vec2 ab = IntegrateEnvironmentBrdf(roughness, NdV, sampleCount);
            float* texel = &table.pixels[(static_cast<size_t>(y) * size + x) * 4];
            texel[0] = ab.x;
            texel[1] = ab.y;
            texel[2] = 0.0f;
            texel[3] = 1.0f;
        }
    }
    return table;
}
}
