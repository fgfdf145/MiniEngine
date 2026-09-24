#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace me
{

inline constexpr uint32_t kMaxBloomLevels = 6;

// The sizes of the bloom mip chain for a viewport: half the viewport at the top, each level half the
// one above (rounded down), up to kMaxBloomLevels, stopping before a side would fall under 2. Always
// at least one level, and no level is ever empty.
std::vector<glm::uvec2> BuildBloomMipChain(glm::uvec2 extent);
}
