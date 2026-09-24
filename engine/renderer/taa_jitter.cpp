#include "taa_jitter.h"

namespace me
{

namespace
{
// The radical inverse of index in the given base: its digits mirrored about the point.
float Halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0)
    {
        result += static_cast<float>(index % base) * fraction;
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}
}

glm::vec2 TaaJitterPixels(uint32_t frameIndex)
{
    // Halton(2, 3) from index 1: index 0 is (0, 0), which would put one sample in eight on the
    // pixel's corner. Eight points of this sequence cover the pixel evenly, and the pattern
    // repeats before the eye can pick out a cycle.
    const uint32_t index = frameIndex % kTaaJitterSequenceLength + 1;
    return glm::vec2(Halton(index, 2), Halton(index, 3)) - 0.5f;
}

glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 jitterPixels, glm::uvec2 extent)
{
    // A perspective projection has clip.w = -z, so adding d to P[2][0] moves NDC x by -d at every
    // depth; the minus signs turn that into a shift of +jitter pixels. One pixel is 2 / extent in
    // NDC. Depth (row 2) and w are untouched.
    glm::mat4 jittered = projection;
    jittered[2][0] -= 2.0f * jitterPixels.x / static_cast<float>(extent.x);
    jittered[2][1] -= 2.0f * jitterPixels.y / static_cast<float>(extent.y);
    return jittered;
}
}
