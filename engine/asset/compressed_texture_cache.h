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
inline constexpr uint32_t kTextureCacheVersion = 2;

// The texture cache is shared by every checkout on the machine and only grows, so a trim keeps it
// under this size, least recently used files first.
inline constexpr uint64_t kTextureCacheBudgetBytes = 32ull << 30;

// Where compressed textures are cached: EnginePaths::CacheRoot() / "textures".
std::filesystem::path DefaultTextureCacheDirectory();

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

struct TextureCacheTrim
{
    uint64_t bytesBefore = 0;
    uint64_t bytesDeleted = 0;
    size_t filesDeleted = 0;
};

// Keeps a cache directory under budgetBytes. When it is larger, deletes the least recently used
// cache files (by last write time, which a cache hit refreshes) until it is under three quarters of
// the budget, so trims stay rare. Also deletes temporary files older than a day, which a process
// that died mid-write left behind. Files another process holds open are skipped. Never throws.
TextureCacheTrim TrimCompressedTextureCache(const std::filesystem::path& cacheDirectory, uint64_t budgetBytes);

struct CompressedTextureLoad
{
    CompressedTexture texture;
    bool cacheHit = false;
};

// The compressed form of an image file: from the cache when present, otherwise decoded,
// compressed and cached. A failed cache write is logged and does not fail the load. Decode and
// compression failures throw. Thread-safe for distinct and identical files alike. A hit refreshes
// the cache file's last write time (at most once a day), which TrimCompressedTextureCache reads as
// its last use.
CompressedTextureLoad LoadOrCompressTexture(
    const std::filesystem::path& imagePath,
    TextureUsage usage,
    const std::filesystem::path& cacheDirectory);
}
