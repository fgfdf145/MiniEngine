#include "third_party/gt7/gt7_tone_mapping_reference.h"

#include <engine/renderer/camera.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

// The tone mapping shader's operator, compiled as C++. gt7_tonemap.glsl is written in the subset
// of GLSL that GLM also accepts, so the test exercises the same source glslc compiles.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/gt7_tonemap.glsl>
}

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

glm::vec3 ReferenceSdr(const gt7_reference::GT7ToneMapping& reference, const glm::vec3& rec2020)
{
    const float input[3] = {rec2020.x, rec2020.y, rec2020.z};
    float output[3] = {};
    reference.applyToneMapping(input, output);
    return glm::vec3(output[0], output[1], output[2]);
}

float MaxAbsDifference(const glm::vec3& lhs, const glm::vec3& rhs)
{
    const glm::vec3 difference = glm::abs(lhs - rhs);
    return std::max({difference.x, difference.y, difference.z});
}

void PortMatchesReferenceOnSampleInputs()
{
    gt7_reference::GT7ToneMapping reference;
    reference.initializeAsSDR();
    const shader::Gt7ToneMapping port = shader::Gt7InitializeAsSdr();

    // The inputs the reference sample's own harness prints.
    const std::array<glm::vec3, 3> inputs = {
        glm::vec3(0.5f, 1.23f, 0.75f),
        glm::vec3(12.3f, 34.3f, 56.9f),
        glm::vec3(1504.7f, 64.51f, 0.5f)};
    for (const glm::vec3& input : inputs)
    {
        Require(
            MaxAbsDifference(shader::Gt7ApplyToneMapping(port, input), ReferenceSdr(reference, input)) <= 1e-6f,
            "the port must match the reference on the sample's inputs");
    }
}

void PortMatchesReferenceAcrossTheRange()
{
    gt7_reference::GT7ToneMapping reference;
    reference.initializeAsSDR();
    const shader::Gt7ToneMapping port = shader::Gt7InitializeAsSdr();

    // Log-uniform per channel from deep shadow to far past the shoulder, so saturated colors,
    // near-grays and every branch of the curve are all covered.
    std::mt19937 generator(7u);
    std::uniform_real_distribution<float> exponent(-14.0f, 8.0f);
    for (int sample = 0; sample < 100000; ++sample)
    {
        const glm::vec3 input(
            std::exp2(exponent(generator)),
            std::exp2(exponent(generator)),
            std::exp2(exponent(generator)));
        Require(
            MaxAbsDifference(shader::Gt7ApplyToneMapping(port, input), ReferenceSdr(reference, input)) <= 1e-6f,
            "the port must match the reference across the input range");
    }

    Require(
        MaxAbsDifference(shader::Gt7ApplyToneMapping(port, glm::vec3(0.0f)), ReferenceSdr(reference, glm::vec3(0.0f))) <= 1e-6f,
        "the port must match the reference at black");
}

void PrimariesConversionsAreInverse()
{
    std::mt19937 generator(11u);
    std::uniform_real_distribution<float> channel(0.0f, 4.0f);
    for (int sample = 0; sample < 1000; ++sample)
    {
        const glm::vec3 color(channel(generator), channel(generator), channel(generator));
        const glm::vec3 roundTrip = color * shader::kRec709ToRec2020 * shader::kRec2020ToRec709;
        Require(MaxAbsDifference(roundTrip, color) <= 1e-4f, "Rec.709 -> Rec.2020 -> Rec.709 must round-trip");
    }

    // Both primaries share the D65 white point, so white stays white.
    const glm::vec3 white = glm::vec3(1.0f) * shader::kRec709ToRec2020;
    Require(MaxAbsDifference(white, glm::vec3(1.0f)) <= 1e-5f, "white must map to white");
}

void BlackStaysBlackAndHighlightsReachWhite()
{
    Require(
        MaxAbsDifference(shader::TonemapExposedRec709(glm::vec3(0.0f)), glm::vec3(0.0f)) <= 1e-6f,
        "black must stay black");
    Require(
        MaxAbsDifference(shader::TonemapExposedRec709(glm::vec3(4.0f)), glm::vec3(1.0f)) <= 1e-4f,
        "a gray well past sensor saturation must reach display white");

    float previous = -1.0f;
    for (float exposed = 0.0f; exposed <= 8.0f; exposed += 0.01f)
    {
        const glm::vec3 mapped = shader::TonemapExposedRec709(glm::vec3(exposed));
        Require(mapped.x >= 0.0f && mapped.x <= 1.0f, "output must stay inside [0, 1]");
        Require(mapped.x >= previous - 1e-6f, "a brighter gray must never map darker");
        previous = mapped.x;
    }
}

void MidGrayStaysWhereReinhardPutIt()
{
    // An averaged scene exposed to EV100 sits at 1 / 9.6 of sensor saturation (reflected-light
    // meter constant 12.5 against the 1.2 * 2^EV saturation point). kExposedToGt7FrameBuffer was
    // chosen so that point displays as it did under the previous Reinhard operator.
    const float midGray = 1.0f / 9.6f;
    const float gt7 = shader::TonemapExposedRec709(glm::vec3(midGray)).x;
    const float reinhard = midGray / (1.0f + midGray);
    Require(std::abs(gt7 - reinhard) <= 0.005f, "mid gray must display within 0.005 of Reinhard");
}

void BackgroundConstantMatchesTheOperator()
{
    const glm::vec3 displayed = shader::TonemapExposedRec709(kViewportBackgroundExposed);
    Require(
        MaxAbsDifference(displayed, kViewportBackgroundDisplayLinear) <= 1e-4f,
        "kViewportBackgroundExposed must tone map to the viewport background");
}
}

int main()
{
    try
    {
        PortMatchesReferenceOnSampleInputs();
        PortMatchesReferenceAcrossTheRange();
        PrimariesConversionsAreInverse();
        BlackStaysBlackAndHighlightsReachWhite();
        MidGrayStaysWhereReinhardPutIt();
        BackgroundConstantMatchesTheOperator();
    }
    catch (const std::exception& error)
    {
        std::cerr << "tonemap tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "tonemap tests passed\n";
    return 0;
}
