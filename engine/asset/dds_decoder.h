#pragma once

#include "texture_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace me
{

// DirectDraw Surface files, decoded on the CPU. The engine renders PNG/JPEG/KTX2 textures; DDS
// is what imported formats that were authored for Direct3D (Assetto Corsa's .kn5) carry, so an
// importer decodes it once and writes a file the rest of the engine reads.
namespace DdsDecoder
{
// True when the bytes start with the "DDS " magic and hold a whole header.
bool IsDds(const std::uint8_t* bytes, size_t size);

// The base level (the first face of a cube map, the first slice of an array) as RGBA8, rows
// top-down. Reads BC1 (DXT1), BC2 (DXT2/3), BC3 (DXT4/5), BC4 (ATI1), BC5 (ATI2), BC7, and
// uncompressed RGB(A), BGR(A), luminance, luminance-alpha and alpha-only surfaces described by
// their channel masks. BC4 and BC5 decode to grey and to red-green; a missing alpha is 255.
// Throws std::runtime_error, naming `source`, for a truncated file or an unsupported format.
TextureData Decode(const std::uint8_t* bytes, size_t size, const std::string& source);
}
}
