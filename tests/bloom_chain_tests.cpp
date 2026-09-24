#include <engine/renderer/bloom_chain.h>

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

void HalvesFromHalfResolution()
{
    const std::vector<glm::uvec2> chain = BuildBloomMipChain(glm::uvec2(667u, 541u));
    Require(chain.size() == kMaxBloomLevels, "a normal viewport gets every level, got " + std::to_string(chain.size()));
    Require(chain[0] == glm::uvec2(333u, 270u), "the top level is half the viewport, rounded down");
    for (size_t level = 1; level < chain.size(); ++level)
    {
        Require(chain[level] == glm::max(chain[level - 1] / 2u, glm::uvec2(1u)), "each level halves the one above");
    }
}

void StopsBeforeASideFallsUnderTwo()
{
    // 64 x 12: half is 32 x 6, then 16 x 3; 8 x 1 would put a side under 2.
    const std::vector<glm::uvec2> chain = BuildBloomMipChain(glm::uvec2(64u, 12u));
    Require(chain.size() == 2, "a thin viewport stops early, got " + std::to_string(chain.size()) + " levels");
    Require(chain.back() == glm::uvec2(16u, 3u), "the last level keeps both sides at 2 or more");
}

void TinyViewportsStillGetALevel()
{
    for (const glm::uvec2 extent : {glm::uvec2(1u, 1u), glm::uvec2(2u, 2u), glm::uvec2(3u, 1u), glm::uvec2(0u, 0u)})
    {
        const std::vector<glm::uvec2> chain = BuildBloomMipChain(extent);
        Require(chain.size() == 1, "a tiny viewport gets exactly one level");
        Require(chain[0].x >= 1u && chain[0].y >= 1u, "no level is ever empty");
    }
}
}

int main()
{
    try
    {
        HalvesFromHalfResolution();
        StopsBeforeASideFallsUnderTwo();
        TinyViewportsStillGetALevel();
    }
    catch (const std::exception& error)
    {
        std::cerr << "bloom chain tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "bloom chain tests passed\n";
    return 0;
}
