#include "restir_pairing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <stdexcept>

namespace me
{

uint32_t PairingShuffleCount(float sigma)
{
    const float s = std::max(sigma, 0.8f);
    const float count = s * s / 2.0f + 1.46f / s + 1.76f / (s * s) + 0.656f / (s * s * s) + 0.5f;
    return std::max(1u, static_cast<uint32_t>(std::floor(count)));
}

std::vector<PairingOffset> BuildPairingTexture(uint32_t size, float sigma, uint32_t seed)
{
    if (size < 2 || size % 2 != 0 || size > 254)
    {
        throw std::invalid_argument("BuildPairingTexture: size must be even and in [2, 254]");
    }
    const uint32_t half = size / 2;
    // Each pixel's link index; two pixels share each index.
    std::vector<uint32_t> links(static_cast<size_t>(size) * size);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            links[y * size + x] = y * half + x / 2;
        }
    }

    std::mt19937 random(seed);
    const uint32_t rounds = PairingShuffleCount(sigma);
    for (uint32_t round = 0; round < rounds; ++round)
    {
        const uint32_t offset = round & 1u;
        for (uint32_t by = 0; by < half; ++by)
        {
            for (uint32_t bx = 0; bx < half; ++bx)
            {
                std::array<size_t, 4> cells{};
                for (uint32_t corner = 0; corner < 4; ++corner)
                {
                    const uint32_t x = (2 * bx + offset + (corner & 1u)) % size;
                    const uint32_t y = (2 * by + offset + (corner >> 1)) % size;
                    cells[corner] = static_cast<size_t>(y) * size + x;
                }
                std::array<uint32_t, 4> values = {links[cells[0]], links[cells[1]], links[cells[2]], links[cells[3]]};
                std::shuffle(values.begin(), values.end(), random);
                for (uint32_t corner = 0; corner < 4; ++corner)
                {
                    links[cells[corner]] = values[corner];
                }
            }
        }
    }

    // The index table: the two pixels holding each link index.
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    std::vector<std::array<uint32_t, 2>> holders(static_cast<size_t>(half) * size, {kNone, kNone});
    for (uint32_t pixel = 0; pixel < size * size; ++pixel)
    {
        std::array<uint32_t, 2>& slot = holders[links[pixel]];
        slot[slot[0] == kNone ? 0 : 1] = pixel;
    }

    const int wrap = static_cast<int>(size);
    const auto shortest = [&](int delta)
    {
        if (delta > wrap / 2)
        {
            delta -= wrap;
        }
        else if (delta < -wrap / 2)
        {
            delta += wrap;
        }
        return delta;
    };
    std::vector<PairingOffset> offsets(static_cast<size_t>(size) * size);
    for (uint32_t pixel = 0; pixel < size * size; ++pixel)
    {
        const std::array<uint32_t, 2>& slot = holders[links[pixel]];
        const uint32_t partner = slot[0] == pixel ? slot[1] : slot[0];
        const int dx = shortest(static_cast<int>(partner % size) - static_cast<int>(pixel % size));
        const int dy = shortest(static_cast<int>(partner / size) - static_cast<int>(pixel / size));
        offsets[pixel] = PairingOffset{static_cast<int8_t>(dx), static_cast<int8_t>(dy)};
    }
    return offsets;
}
}
