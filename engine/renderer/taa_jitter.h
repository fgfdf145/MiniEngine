#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace me
{

// How many frames the sub-pixel jitter takes to repeat.
inline constexpr uint32_t kTaaJitterSequenceLength = 8;

// This frame's sub-pixel offset for temporal anti-aliasing, in pixels, each component in
// (-0.5, 0.5).
glm::vec2 TaaJitterPixels(uint32_t frameIndex);

// The projection with its image shifted by jitterPixels on a viewport of this extent.
glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 jitterPixels, glm::uvec2 extent);
}
