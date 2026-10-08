#include "spirv_patch.h"

#include <bit>
#include <unordered_set>

namespace me
{

namespace
{
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kHeaderWords = 5;
constexpr uint32_t kOpSpecConstant = 50;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationSpecId = 1;
}

bool PatchSpecConstantFloat(std::vector<uint32_t>& spirv, uint32_t specId, float value)
{
    if (spirv.size() < kHeaderWords || spirv[0] != kSpirvMagic)
    {
        return false;
    }
    // Decorations come before the constants they decorate (the module layout's order), so one pass
    // finds the ids first and patches the constant after.
    std::unordered_set<uint32_t> ids;
    bool patched = false;
    for (size_t at = kHeaderWords; at < spirv.size();)
    {
        const uint32_t wordCount = spirv[at] >> 16;
        const uint32_t opcode = spirv[at] & 0xFFFFu;
        if (wordCount == 0 || at + wordCount > spirv.size())
        {
            return false;
        }
        if (opcode == kOpDecorate && wordCount >= 4 && spirv[at + 2] == kDecorationSpecId && spirv[at + 3] == specId)
        {
            ids.insert(spirv[at + 1]);
        }
        // OpSpecConstant: result type, result id, then the value (one word for a 32-bit float).
        else if (opcode == kOpSpecConstant && wordCount == 4 && ids.contains(spirv[at + 2]))
        {
            spirv[at + 3] = std::bit_cast<uint32_t>(value);
            patched = true;
        }
        at += wordCount;
    }
    return patched;
}
}
