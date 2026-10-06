#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace me
{

// How many frames the sub-pixel jitter takes to repeat when every output pixel is rendered.
inline constexpr uint32_t kTaaJitterSequenceLength = 8;

// How many frames the jitter should take to repeat when renderExtent is upscaled to outputExtent:
// eight samples for every output pixel, so the base length times the ratio of their pixel counts
// (DLSS programming guide 3.7.1.1: 18 for Quality, 32 for Performance, 72 for Ultra Performance).
uint32_t TaaJitterPhaseCount(glm::uvec2 renderExtent, glm::uvec2 outputExtent);

// The mip bias for material textures while renderExtent is upscaled to outputExtent by a temporal
// upscaler that jitters (DLSS): log2(render width / output width) - 1, so textures are sampled for
// the output's pixels and the accumulated jitter's extra detail (DLSS programming guide 3.5: -2 at
// Performance, -1 at DLAA).
float UpscaleTextureMipBias(glm::uvec2 renderExtent, glm::uvec2 outputExtent);

// This frame's sub-pixel offset for temporal anti-aliasing, in pixels, each component in
// (-0.5, 0.5): the Halton(2, 3) sequence, repeating every phaseCount frames.
glm::vec2 TaaJitterPixels(uint32_t frameIndex, uint32_t phaseCount = kTaaJitterSequenceLength);

// The projection with its image shifted by jitterPixels on a viewport of this extent.
glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 jitterPixels, glm::uvec2 extent);
}
