#pragma once

#include <cstdint>
#include <vector>

namespace me
{

// One texel of a ReSTIR PT pairing texture: the offset, in pixels, to the pixel this one is paired with.
struct PairingOffset
{
    int8_t dx = 0;
    int8_t dy = 0;
};

// How many tiled 2 x 2 shuffles move each link a distance of about sigma pixels (Lin, Kettunen and Wyman
// 2026, "ReSTIR PT Enhanced", equation 3, with its correction for small sigma).
uint32_t PairingShuffleCount(float sigma);

// A size x size pairing texture for paired spatial reuse (the same paper's section 3.1): every texel
// is linked with exactly one other, the link is mutual (texel p's partner q has q's partner p, with the
// texture tiled over the plane), and the offsets follow a normal distribution of about sigma pixels per
// axis. size must be even and at most 254 so the offsets fit in a byte. The texels start linked to their
// horizontal neighbours and are shuffled within 2 x 2 blocks, every other round offset diagonally by one
// and wrapping at the edges; links longer than half the size are then broken by going the other way
// around the tile. Deterministic for a seed. Row-major, y * size + x.
std::vector<PairingOffset> BuildPairingTexture(uint32_t size, float sigma, uint32_t seed);
}
