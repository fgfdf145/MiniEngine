// Marks a vertex shader's Position output Invariant, in place: what GLSL's `invariant gl_Position`
// did before the shaders moved to Slang, which cannot express it
// (docs/design/2026-10-08-slang-shader-migration-design.md). triangle.vert and toon.vert need it: the
// toon pass redraws the characters' opaque surfaces on the depth the geometry pass wrote, and the two
// must agree to the bit.
//
// usage: miniengine_spirv_invariant_position <module.spv>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
constexpr uint32_t kMagic = 0x07230203u;
constexpr uint32_t kOpVariable = 59u;
constexpr uint32_t kOpDecorate = 71u;
constexpr uint32_t kDecorationBuiltIn = 11u;
constexpr uint32_t kDecorationInvariant = 18u;
constexpr uint32_t kBuiltInPosition = 0u;
constexpr uint32_t kStorageClassOutput = 3u;
constexpr size_t kHeaderWords = 5;

bool ReadModule(const char* path, std::vector<uint32_t>& words)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0 || bytes.size() < kHeaderWords * 4)
    {
        return false;
    }
    words.resize(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words[0] == kMagic;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: miniengine_spirv_invariant_position <module.spv>\n");
        return 2;
    }
    std::vector<uint32_t> words;
    if (!ReadModule(argv[1], words))
    {
        std::fprintf(stderr, "%s: not a SPIR-V module\n", argv[1]);
        return 1;
    }

    // The Output variables, then where each BuiltIn Position decoration sits and whether its
    // variable is already Invariant.
    std::vector<uint32_t> outputs;
    for (size_t at = kHeaderWords; at < words.size();)
    {
        const uint32_t count = words[at] >> 16;
        const uint32_t opcode = words[at] & 0xFFFFu;
        if (count == 0 || at + count > words.size())
        {
            std::fprintf(stderr, "%s: malformed instruction stream\n", argv[1]);
            return 1;
        }
        if (opcode == kOpVariable && count >= 4 && words[at + 3] == kStorageClassOutput)
        {
            outputs.push_back(words[at + 2]);
        }
        at += count;
    }

    size_t insertAt = 0;
    uint32_t position = 0;
    for (size_t at = kHeaderWords; at < words.size(); at += words[at] >> 16)
    {
        const uint32_t count = words[at] >> 16;
        if ((words[at] & 0xFFFFu) == kOpDecorate && count == 4 && words[at + 2] == kDecorationBuiltIn &&
            words[at + 3] == kBuiltInPosition &&
            std::find(outputs.begin(), outputs.end(), words[at + 1]) != outputs.end())
        {
            position = words[at + 1];
            insertAt = at + count;
        }
    }
    for (size_t at = kHeaderWords; at < words.size() && position != 0; at += words[at] >> 16)
    {
        if ((words[at] & 0xFFFFu) == kOpDecorate && (words[at] >> 16) == 3 && words[at + 1] == position &&
            words[at + 2] == kDecorationInvariant)
        {
            return 0; // already invariant
        }
    }
    if (position == 0)
    {
        std::fprintf(stderr, "%s: no Position output to make invariant\n", argv[1]);
        return 1;
    }

    const uint32_t decorate[] = {(3u << 16) | kOpDecorate, position, kDecorationInvariant};
    words.insert(words.begin() + static_cast<std::ptrdiff_t>(insertAt), std::begin(decorate), std::end(decorate));
    std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    if (!file)
    {
        std::fprintf(stderr, "%s: could not write the module\n", argv[1]);
        return 1;
    }
    return 0;
}
