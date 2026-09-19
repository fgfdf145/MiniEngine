#pragma once

#include "texture_loader.h"

#include <cstdint>
#include <vector>

namespace me
{

// What a material slot uses a texture for. It decides how the mip chain is filtered and which
// block format stores it. The numeric values are part of the compressed texture cache key.
enum class TextureUsage : uint32_t
{
    // Base colour and emissive: sRGB-encoded, alpha meaningful (cutouts, blending).
    Color = 0,
    // Tangent-space normal maps: only the x and y a unit vector needs.
    Normal = 1,
    // Metallic, roughness, occlusion, blend masks: linear, and often several of them packed into
    // the channels of one file, so every channel is kept.
    Data = 2
};

// The numeric values are stored in compressed texture cache files.
enum class CompressedTextureFormat : uint32_t
{
    Bc7Srgb = 1,
    Bc7Unorm = 2,
    Bc5Unorm = 3
};

struct CompressedTextureLevel
{
    // Pixel size; the blocks cover it rounded up to whole 4x4 blocks.
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> blocks;
};

struct CompressedTexture
{
    CompressedTextureFormat format = CompressedTextureFormat::Bc7Srgb;
    // Level 0 first, down to 1x1.
    std::vector<CompressedTextureLevel> levels;
};

// BC7 and BC5 both spend 16 bytes on each 4x4 block.
inline constexpr uint32_t kCompressedBlockBytes = 16;

CompressedTextureFormat FormatForUsage(TextureUsage usage);

// Blocks needed to cover a row or column of this many pixels.
inline uint32_t BlockCount(uint32_t pixels)
{
    return (pixels + 3) / 4;
}

// The RGBA8 mip chain of an image, level 0 first, each level half the previous one (rounded down,
// at least 1) until 1x1. Colour is filtered in linear light; normals and data as stored.
std::vector<TextureData> BuildMipChain(const TextureData& image, TextureUsage usage);

// Builds the mip chain and encodes every level in the usage's format. Thread-safe: textures may
// be compressed in parallel. Throws std::runtime_error for an invalid image.
CompressedTexture CompressTexture(const TextureData& image, TextureUsage usage);
}
