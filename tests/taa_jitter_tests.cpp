#include <engine/renderer/camera.h>
#include <engine/renderer/taa_jitter.h>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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

void OffsetsAreSubPixelAndCycle()
{
    glm::vec2 sum(0.0f);
    std::set<std::pair<float, float>> distinct;
    for (uint32_t frame = 0; frame < kTaaJitterSequenceLength; ++frame)
    {
        const glm::vec2 offset = TaaJitterPixels(frame);
        Require(offset.x > -0.5f && offset.x < 0.5f && offset.y > -0.5f && offset.y < 0.5f,
                "frame " + std::to_string(frame) + " jitters within the pixel");
        const glm::vec2 again = TaaJitterPixels(frame + kTaaJitterSequenceLength);
        Require(offset == again, "the sequence repeats every cycle");
        distinct.insert({offset.x, offset.y});
        sum += offset;
    }
    Require(distinct.size() == kTaaJitterSequenceLength, "every offset in a cycle is different");
    const glm::vec2 mean = sum / static_cast<float>(kTaaJitterSequenceLength);
    Require(std::fabs(mean.x) < 1.0f / 16.0f && std::fabs(mean.y) < 1.0f / 16.0f,
            "a cycle is centred on the pixel, mean (" + std::to_string(mean.x) + ", " + std::to_string(mean.y) + ")");
}

// Where a view-space point lands, in pixels from the top left, through a projection.
glm::vec2 PixelOf(const glm::mat4& projection, const glm::vec3& viewPosition, glm::uvec2 extent)
{
    const glm::vec4 clip = projection * glm::vec4(viewPosition, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return (ndc * 0.5f + 0.5f) * glm::vec2(extent);
}

void JitterMovesEveryPointByTheOffset()
{
    Camera camera{};
    const glm::uvec2 extent(1221u, 786u);
    // The renderer's projection: Y flipped, zero-to-one depth.
    const glm::mat4 projection = camera.GetProjectionMatrix(RenderExtent{extent.x, extent.y}, true, true);
    const glm::vec2 jitter(0.3125f, -0.1875f);
    const glm::mat4 jittered = JitterProjection(projection, jitter, extent);

    for (const glm::vec3& point : {glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(2.0f, -1.0f, -7.5f), glm::vec3(-30.0f, 12.0f, -90.0f)})
    {
        const glm::vec2 shift = PixelOf(jittered, point, extent) - PixelOf(projection, point, extent);
        Require(std::fabs(shift.x - jitter.x) < 1e-3f && std::fabs(shift.y - jitter.y) < 1e-3f,
                "a point at depth " + std::to_string(-point.z) + " moves by the jitter, got (" + std::to_string(shift.x) + ", " +
                    std::to_string(shift.y) + ")");
        const glm::vec4 plainClip = projection * glm::vec4(point, 1.0f);
        const glm::vec4 jitteredClip = jittered * glm::vec4(point, 1.0f);
        Require(std::fabs(plainClip.z / plainClip.w - jitteredClip.z / jitteredClip.w) < 1e-6f, "jitter leaves depth alone");
    }
    Require(JitterProjection(projection, glm::vec2(0.0f), extent) == projection, "zero jitter is the identity");
}
}

int main()
{
    try
    {
        OffsetsAreSubPixelAndCycle();
        JitterMovesEveryPointByTheOffset();
    }
    catch (const std::exception& error)
    {
        std::cerr << "TAA jitter tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "TAA jitter tests passed\n";
    return 0;
}
