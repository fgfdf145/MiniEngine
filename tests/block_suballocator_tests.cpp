#include <engine/renderer/block_suballocator.h>

#include <iostream>
#include <stdexcept>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void AlignsAndPacks()
{
    BlockSuballocator block(1024);
    const auto a = block.Allocate(10, 1);
    const auto b = block.Allocate(16, 256);
    Require(a && *a == 0, "the first range starts the block");
    Require(b && *b == 256, "an aligned range starts on its alignment");
    // The padding between them stays usable.
    const auto c = block.Allocate(100, 4);
    Require(c && *c == 12, "padding in front of an aligned range is handed out again");
    Require(block.UsedBytes() == 126, "used bytes add up");
}

void RefusesWhatDoesNotFit()
{
    BlockSuballocator block(256);
    Require(block.Allocate(200, 1).has_value(), "a range that fits is given");
    Require(!block.Allocate(100, 1).has_value(), "a range larger than what is left is refused");
    Require(!block.Allocate(0, 1).has_value(), "an empty range is refused");
}

void MergesFreedNeighbours()
{
    BlockSuballocator block(300);
    const uint64_t a = *block.Allocate(100, 1);
    const uint64_t b = *block.Allocate(100, 1);
    const uint64_t c = *block.Allocate(100, 1);
    Require(!block.Allocate(1, 1).has_value(), "the block is full");
    block.Free(a, 100);
    block.Free(c, 100);
    Require(block.FreeRangeCount() == 2, "two holes on either side of the middle range");
    block.Free(b, 100);
    Require(block.Empty() && block.FreeRangeCount() == 1, "freeing the middle merges everything back");
    Require(block.Allocate(300, 1) == 0u, "the whole block can be handed out again");
}

void ReusesHolesUnderChurn()
{
    BlockSuballocator block(64 * 1024);
    std::vector<uint64_t> offsets;
    for (int i = 0; i < 64; ++i)
    {
        offsets.push_back(*block.Allocate(1000, 16));
    }
    for (size_t i = 0; i < offsets.size(); i += 2)
    {
        block.Free(offsets[i], 1000);
    }
    for (int i = 0; i < 32; ++i)
    {
        Require(block.Allocate(1000, 16).has_value(), "freed holes take same-sized ranges again");
    }
}

void RejectsBadFrees()
{
    BlockSuballocator block(100);
    const uint64_t a = *block.Allocate(50, 1);
    block.Free(a, 50);
    bool threw = false;
    try
    {
        block.Free(a, 50);
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }
    Require(threw, "freeing a range twice is caught");
}
}

int main()
{
    try
    {
        AlignsAndPacks();
        RefusesWhatDoesNotFit();
        MergesFreedNeighbours();
        ReusesHolesUnderChurn();
        RejectsBadFrees();
    }
    catch (const std::exception& error)
    {
        std::cerr << "block_suballocator tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "block_suballocator tests passed\n";
    return 0;
}
