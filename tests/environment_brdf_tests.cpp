#include <engine/renderer/environment_brdf.h>

#include <algorithm>
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

// Karis' analytic fit of the same table, as pbr_common.glsl's EnvironmentBrdfApprox has it.
glm::vec2 KarisFit(float roughness, float NdV)
{
    const glm::vec4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
    const glm::vec4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
    const glm::vec4 r = roughness * c0 + c1;
    const float a004 = std::min(r.x * r.x, std::exp2(-9.28f * NdV)) * r.x + r.y;
    return glm::vec2(-1.04f, 1.04f) * a004 + glm::vec2(r.z, r.w);
}

void MirrorHeadOnReflectsEverything()
{
    const glm::vec2 ab = IntegrateEnvironmentBrdf(0.0f, 1.0f, 512);
    Require(std::fabs(ab.x - 1.0f) < 1e-3f && std::fabs(ab.y) < 1e-3f,
            "a mirror seen head on has A = 1, B = 0, got " + std::to_string(ab.x) + ", " + std::to_string(ab.y));
}

// A mirror's half vector is N, so the table is Schlick's Fresnel exactly: B = (1 - N.V)^5, A = 1 - B.
void MirrorIsSchlickFresnel()
{
    for (float NdV = 0.1f; NdV <= 1.0f; NdV += 0.1f)
    {
        const glm::vec2 ab = IntegrateEnvironmentBrdf(0.0f, NdV, 64);
        const float fresnel = std::pow(1.0f - NdV, 5.0f);
        Require(std::fabs(ab.y - fresnel) < 1e-4f && std::fabs(ab.x - (1.0f - fresnel)) < 1e-4f,
                "a mirror at N.V " + std::to_string(NdV) + " is Schlick's Fresnel, got (" + std::to_string(ab.x) + ", " + std::to_string(ab.y) + ")");
    }
}

// Karis' analytic fit is only a coarse approximation of this table: it under-reflects near-mirrors
// seen head on by about 0.14 (0.86 against 0.99 at roughness 0.25), which the mirror test above
// shows the integral gets right. So it bounds the table loosely, as a same-family check, and the
// exact properties carry the test.
void ConservesEnergyAndConverges()
{
    float previousHeadOn = 2.0f;
    for (float roughness = 0.1f; roughness <= 1.0f; roughness += 0.1f)
    {
        const glm::vec2 headOn = IntegrateEnvironmentBrdf(roughness, 1.0f, 512);
        Require(headOn.x + headOn.y < previousHeadOn, "head on, rougher surfaces reflect less");
        previousHeadOn = headOn.x + headOn.y;
        for (float NdV = 0.1f; NdV <= 1.0f; NdV += 0.1f)
        {
            const glm::vec2 ab = IntegrateEnvironmentBrdf(roughness, NdV, 512);
            const glm::vec2 reference = IntegrateEnvironmentBrdf(roughness, NdV, 4096);
            const glm::vec2 fit = KarisFit(roughness, NdV);
            Require(ab.x >= 0.0f && ab.y >= 0.0f && ab.x + ab.y <= 1.0f + 1e-3f, "the table never reflects more than it receives");
            Require(glm::length(ab - reference) < 0.01f, "512 samples are within 0.01 of 4096");
            Require(std::fabs(ab.x - fit.x) < 0.2f && std::fabs(ab.y - fit.y) < 0.2f,
                    "roughness " + std::to_string(roughness) + ", N.V " + std::to_string(NdV) + " strays from Karis' fit");
        }
    }
}

void TableSamplesTexelCentres()
{
    const FloatTextureData table = BuildEnvironmentBrdfLut(8, 64);
    Require(table.IsValid() && table.width == 8 && table.height == 8, "the table is size x size");
    // Texel (x, y): N.V = (x + 0.5) / 8 across, roughness = (y + 0.5) / 8 down.
    const glm::vec2 expected = IntegrateEnvironmentBrdf(5.5f / 8.0f, 2.5f / 8.0f, 64);
    const float* texel = &table.pixels[(5 * 8 + 2) * 4];
    Require(std::fabs(texel[0] - expected.x) < 1e-6f && std::fabs(texel[1] - expected.y) < 1e-6f && texel[3] == 1.0f,
            "texel (2, 5) holds roughness 5.5 / 8 at N.V 2.5 / 8");
}

// A white furnace: a perfect conductor (F0 = 1) under uniform light must reflect all of it at
// every roughness and angle once the multiple-scattering term is added back, and no F0 may
// reflect more than it receives. Checked over the table the shader actually samples.
void EnergyCompensationRestoresTheWhiteFurnace()
{
    const FloatTextureData table = BuildEnvironmentBrdfLut(kEnvironmentBrdfLutSize, 64);
    float largestCompensation = 0.0f;
    for (size_t texel = 0; texel < table.pixels.size(); texel += 4)
    {
        const glm::vec2 ab(table.pixels[texel], table.pixels[texel + 1]);
        const glm::vec3 conductor = SpecularEnergyCompensation(glm::vec3(1.0f), ab);
        Require(std::fabs((ab.x + ab.y) * conductor.x - 1.0f) < 1e-4f,
                "F0 = 1 reflects everything once compensated, got " + std::to_string((ab.x + ab.y) * conductor.x));
        for (float f0 = 0.0f; f0 <= 1.0f; f0 += 0.125f)
        {
            const float compensation = SpecularEnergyCompensation(glm::vec3(f0), ab).x;
            Require(compensation >= 1.0f - 1e-6f, "compensation only ever adds energy");
            Require((f0 * ab.x + ab.y) * compensation <= 1.0f + 1e-4f,
                    "F0 " + std::to_string(f0) + " never reflects more than it receives");
            largestCompensation = std::max(largestCompensation, compensation);
        }
    }
    // Only rough, grazing texels lose much energy; the factor must stay bounded there too.
    Require(largestCompensation < 4.0f, "the factor stays bounded, got " + std::to_string(largestCompensation));
}

// A dielectric's F0 is small, so its lobe barely loses energy and the factor stays near 1.
void DielectricsBarelyChange()
{
    const glm::vec2 ab = IntegrateEnvironmentBrdf(1.0f, 0.5f, 512);
    const float compensation = SpecularEnergyCompensation(glm::vec3(0.04f), ab).x;
    Require(compensation > 1.0f && compensation < 1.1f,
            "F0 0.04 at roughness 1 gains under 10%, got " + std::to_string(compensation));
}
}

int main()
{
    try
    {
        MirrorHeadOnReflectsEverything();
        MirrorIsSchlickFresnel();
        ConservesEnergyAndConverges();
        TableSamplesTexelCentres();
        EnergyCompensationRestoresTheWhiteFurnace();
        DielectricsBarelyChange();
    }
    catch (const std::exception& error)
    {
        std::cerr << "environment BRDF tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "environment BRDF tests passed\n";
    return 0;
}
