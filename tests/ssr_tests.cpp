#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// The SSR helpers the trace and the lighting pass use, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/ssr_common.glsl>
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

void SpecularOcclusionFollowsTheAmbientOcclusion()
{
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.25f)
    {
        for (float NdV = 0.05f; NdV <= 1.0f; NdV += 0.2f)
        {
            Require(std::abs(shader::SpecularOcclusion(NdV, 1.0f, roughness) - 1.0f) < 1e-5f, "no AO, no specular occlusion");
            for (float ao = 0.0f; ao <= 1.0f; ao += 0.1f)
            {
                const float so = shader::SpecularOcclusion(NdV, ao, roughness);
                Require(so >= 0.0f && so <= 1.0f, "specular occlusion stays in [0, 1]");
            }
        }
    }
    Require(shader::SpecularOcclusion(0.5f, 0.0f, 1.0f) < 1e-5f, "a fully occluded rough surface reflects nothing");
    // A mirror seen head on keeps more of its reflection than its AO: the lobe is narrow.
    Require(shader::SpecularOcclusion(1.0f, 0.5f, 0.0f) > 0.5f, "a narrow lobe escapes more than the cosine-weighted AO");
}

void EdgeFadeIsOneInsideAndZeroAtTheBorder()
{
    Require(shader::SsrEdgeFade(glm::vec2(0.5f)) == 1.0f, "the centre is fully trusted");
    Require(shader::SsrEdgeFade(glm::vec2(0.2f, 0.8f)) == 1.0f, "inside the margin is fully trusted");
    Require(shader::SsrEdgeFade(glm::vec2(0.0f, 0.5f)) == 0.0f, "the left border is not trusted");
    Require(shader::SsrEdgeFade(glm::vec2(0.5f, 1.0f)) == 0.0f, "the bottom border is not trusted");
    const float halfway = shader::SsrEdgeFade(glm::vec2(0.05f, 0.5f));
    Require(halfway > 0.0f && halfway < 1.0f, "the margin fades");
}

void RoughnessFadeHandsOverToTheEnvironment()
{
    Require(shader::SsrRoughnessFade(0.2f, 0.6f) == 1.0f, "glossy surfaces use the trace");
    Require(shader::SsrRoughnessFade(0.4f, 0.6f) == 1.0f, "the fade starts at 0.4");
    Require(shader::SsrRoughnessFade(0.6f, 0.6f) == 0.0f, "the maximum roughness is all environment");
    Require(shader::SsrRoughnessFade(0.9f, 0.6f) == 0.0f, "rougher still is all environment");
    const float middle = shader::SsrRoughnessFade(0.5f, 0.6f);
    Require(middle > 0.0f && middle < 1.0f, "between the two it fades");
}

void VndfSamplesStayAroundTheNormal()
{
    std::mt19937 generator(5u);
    std::uniform_real_distribution<float> unit(0.0f, 0.999f);
    const glm::vec3 N = glm::normalize(glm::vec3(0.3f, 0.9f, 0.2f));
    const glm::vec3 V = glm::normalize(glm::vec3(-0.2f, 0.7f, 0.6f));
    for (float roughness : {0.0f, 0.1f, 0.5f, 1.0f})
    {
        for (int sample = 0; sample < 2000; ++sample)
        {
            const glm::vec3 H = shader::SampleGgxVisibleNormal(N, V, roughness, glm::vec2(unit(generator), unit(generator)));
            Require(std::abs(glm::length(H) - 1.0f) < 1e-3f, "the half vector is a unit vector");
            Require(glm::dot(H, N) > -1e-4f, "the half vector is in the normal's hemisphere");
            if (roughness == 0.0f)
            {
                Require(glm::length(H - N) < 1e-3f, "a mirror's half vector is its normal");
            }
        }
    }
}
}

int main()
{
    try
    {
        SpecularOcclusionFollowsTheAmbientOcclusion();
        EdgeFadeIsOneInsideAndZeroAtTheBorder();
        RoughnessFadeHandsOverToTheEnvironment();
        VndfSamplesStayAroundTheNormal();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ssr tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ssr tests passed\n";
    return 0;
}
