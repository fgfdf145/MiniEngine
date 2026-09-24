#include "bloom_chain.h"

namespace me
{

std::vector<glm::uvec2> BuildBloomMipChain(glm::uvec2 extent)
{
    // The top level exists whatever the viewport, so a collapsed editor panel still has something
    // to downsample into.
    std::vector<glm::uvec2> chain{glm::max(extent / 2u, glm::uvec2(1u))};
    while (chain.size() < kMaxBloomLevels)
    {
        const glm::uvec2 next = glm::max(chain.back() / 2u, glm::uvec2(1u));
        // A side under 2 has nothing left to blur along it.
        if (next.x < 2u || next.y < 2u)
        {
            break;
        }
        chain.push_back(next);
    }
    return chain;
}
}
