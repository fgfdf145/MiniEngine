#pragma once

#include "texture_compression.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace me
{

// Bump whenever the encoder settings or the cache file layout change: every existing cache file
// then becomes a miss and is rebuilt.
inline constexpr uint32_t kTextureCacheVersion = 1;

// Identifies one compressed form of one image file: its canonical path, size, last write time and
// usage, plus kTextureCacheVersion. Editing, replacing or touching the file changes the key.
// Throws std::filesystem::filesystem_error when the file cannot be inspected.
std::string BuildCompressedTextureKey(const std::filesystem::path& imagePath, TextureUsage usage);

// Where the texture with this key is cached: a hash of the key, so the name is short and stable.
std::filesystem::path CompressedTextureCacheFile(const std::filesystem::path& cacheDirectory, const std::string& key);

// Reads a cache file. Anything unexpected (missing, truncated, another format version, a key other
// than the one asked for) is std::nullopt, which callers treat as a miss.
std::optional<CompressedTexture> ReadCompressedTexture(const std::filesystem::path& file, const std::string& key);

// Writes through a temporary file and a rename, so a reader never sees a partial file even when two
// editors cache the same texture at once. Returns false, leaving no temporary behind, on failure.
bool WriteCompressedTexture(const std::filesystem::path& file, const std::string& key, const CompressedTexture& texture);

struct CompressedTextureLoad
{
    CompressedTexture texture;
    bool cacheHit = false;
};

// The compressed form of an image file: from the cache when present, otherwise decoded,
// compressed and cached. A failed cache write is logged and does not fail the load. Decode and
// compression failures throw. Thread-safe for distinct and identical files alike.
CompressedTextureLoad LoadOrCompressTexture(
    const std::filesystem::path& imagePath,
    TextureUsage usage,
    const std::filesystem::path& cacheDirectory);
}
