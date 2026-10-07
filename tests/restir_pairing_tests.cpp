#include <engine/renderer/restir_pairing.h>

#include <cmath>
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

void ShuffleCountFollowsTheFit()
{
    Require(PairingShuffleCount(16.0f) == 128, "sigma 16 takes 128 shuffles");
    Require(PairingShuffleCount(4.0f) == 8, "sigma 4 takes 8 shuffles");
    Require(PairingShuffleCount(0.1f) == PairingShuffleCount(0.8f), "sigma below 0.8 is 0.8");
}

void LinksAreMutualOnTheTiledPlane()
{
    for (const uint32_t size : {254u, 230u, 210u})
    {
        const std::vector<PairingOffset> texture = BuildPairingTexture(size, 16.0f, size);
        Require(texture.size() == size * size, "one offset per texel");
        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                const PairingOffset offset = texture[y * size + x];
                Require(offset.dx != 0 || offset.dy != 0, "no texel is linked with itself");
                const uint32_t px = static_cast<uint32_t>(static_cast<int>(x) + offset.dx + static_cast<int>(size)) % size;
                const uint32_t py = static_cast<uint32_t>(static_cast<int>(y) + offset.dy + static_cast<int>(size)) % size;
                const PairingOffset back = texture[py * size + px];
                Require(back.dx == -offset.dx && back.dy == -offset.dy,
                        "the partner of texel (" + std::to_string(x) + ", " + std::to_string(y) + ") links back to it");
            }
        }
    }
}

void OffsetsSpreadAboutSigma()
{
    const uint32_t size = 254;
    const std::vector<PairingOffset> texture = BuildPairingTexture(size, 16.0f, 7);
    double sumX = 0.0;
    double sumSquares = 0.0;
    double sumDistance = 0.0;
    for (const PairingOffset& offset : texture)
    {
        const double dx = offset.dx;
        const double dy = offset.dy;
        sumX += dx;
        sumSquares += 0.5 * (dx * dx + dy * dy);
        sumDistance += std::sqrt(dx * dx + dy * dy);
    }
    const double count = static_cast<double>(texture.size());
    const double sigma = std::sqrt(sumSquares / count);
    const double meanDistance = sumDistance / count;
    std::cout << "pairing sigma " << sigma << ", mean distance " << meanDistance << "\n";
    Require(std::abs(sumX / count) < 0.5, "offsets are centred");
    Require(sigma > 14.4 && sigma < 17.6, "the per-axis spread is sigma within 10%");
    // A uniform disk of radius 30 has a mean distance of 20, which sigma 16 matches (the paper's section 7).
    Require(meanDistance > 18.0 && meanDistance < 22.0, "the mean distance matches a 30-pixel disk");
}

void SeedsDiffer()
{
    const std::vector<PairingOffset> a = BuildPairingTexture(64, 4.0f, 1);
    const std::vector<PairingOffset> b = BuildPairingTexture(64, 4.0f, 2);
    const std::vector<PairingOffset> again = BuildPairingTexture(64, 4.0f, 1);
    bool differs = false;
    bool same = true;
    for (size_t i = 0; i < a.size(); ++i)
    {
        differs = differs || a[i].dx != b[i].dx || a[i].dy != b[i].dy;
        same = same && a[i].dx == again[i].dx && a[i].dy == again[i].dy;
    }
    Require(differs, "another seed gives another texture");
    Require(same, "a seed gives the same texture every time");
}
}

int main()
{
    try
    {
        ShuffleCountFollowsTheFit();
        LinksAreMutualOnTheTiledPlane();
        OffsetsSpreadAboutSigma();
        SeedsDiffer();
    }
    catch (const std::exception& error)
    {
        std::cerr << "restir pairing tests failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "restir pairing tests passed\n";
    return 0;
}
