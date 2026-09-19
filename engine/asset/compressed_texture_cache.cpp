#include "compressed_texture_cache.h"

#include <engine/core/log/log.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>

namespace me
{

namespace
{
// File layout, little endian:
//   char[4]  magic "METX"
//   uint32   kTextureCacheVersion
//   uint32   CompressedTextureFormat
//   uint32   level count
//   uint32   key length, then the key bytes
//   per level: uint32 width, uint32 height, uint64 byte count, then the blocks
constexpr std::array<char, 4> kMagic = {'M', 'E', 'T', 'X'};
constexpr uint32_t kMaxLevels = 32;
constexpr uint32_t kMaxKeyLength = 64 * 1024;

uint64_t Fnv1a64(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const char character : text)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename Value>
void WriteValue(std::ofstream& stream, Value value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename Value>
bool ReadValue(std::ifstream& stream, Value& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}

bool IsKnownFormat(uint32_t format)
{
    return format == static_cast<uint32_t>(CompressedTextureFormat::Bc7Srgb) ||
           format == static_cast<uint32_t>(CompressedTextureFormat::Bc7Unorm) ||
           format == static_cast<uint32_t>(CompressedTextureFormat::Bc5Unorm);
}

// A name no other writer in this or another process will pick for the same target.
std::filesystem::path TemporaryPathFor(const std::filesystem::path& file)
{
    static std::atomic<uint64_t> counter{0};
    static const uint64_t processSalt = std::random_device{}();
    std::filesystem::path temporary = file;
    temporary += ".tmp-" + std::to_string(processSalt) + "-" + std::to_string(counter.fetch_add(1));
    return temporary;
}
}

std::string BuildCompressedTextureKey(const std::filesystem::path& imagePath, TextureUsage usage)
{
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(imagePath);
    const uintmax_t size = std::filesystem::file_size(canonical);
    const auto writeTime = std::filesystem::last_write_time(canonical).time_since_epoch().count();
    return canonical.generic_string() + "|" + std::to_string(size) + "|" + std::to_string(writeTime) + "|" +
           std::to_string(static_cast<uint32_t>(usage)) + "|v" + std::to_string(kTextureCacheVersion);
}

std::filesystem::path CompressedTextureCacheFile(const std::filesystem::path& cacheDirectory, const std::string& key)
{
    char name[32] = {};
    std::snprintf(name, sizeof(name), "%016llx.metex", static_cast<unsigned long long>(Fnv1a64(key)));
    return cacheDirectory / name;
}

std::optional<CompressedTexture> ReadCompressedTexture(const std::filesystem::path& file, const std::string& key)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }

    std::array<char, 4> magic{};
    uint32_t version = 0;
    uint32_t format = 0;
    uint32_t levelCount = 0;
    uint32_t keyLength = 0;
    stream.read(magic.data(), magic.size());
    if (!stream || magic != kMagic || !ReadValue(stream, version) || version != kTextureCacheVersion ||
        !ReadValue(stream, format) || !IsKnownFormat(format) ||
        !ReadValue(stream, levelCount) || levelCount == 0 || levelCount > kMaxLevels ||
        !ReadValue(stream, keyLength) || keyLength != key.size() || keyLength > kMaxKeyLength)
    {
        return std::nullopt;
    }

    // The stored key has to match, not just its hash: two keys sharing a file name are a miss.
    std::string storedKey(keyLength, '\0');
    stream.read(storedKey.data(), keyLength);
    if (!stream || storedKey != key)
    {
        return std::nullopt;
    }

    CompressedTexture texture{};
    texture.format = static_cast<CompressedTextureFormat>(format);
    texture.levels.resize(levelCount);
    for (CompressedTextureLevel& level : texture.levels)
    {
        uint64_t byteCount = 0;
        if (!ReadValue(stream, level.width) || !ReadValue(stream, level.height) || !ReadValue(stream, byteCount) ||
            level.width == 0 || level.height == 0 ||
            byteCount != static_cast<uint64_t>(BlockCount(level.width)) * BlockCount(level.height) * kCompressedBlockBytes)
        {
            return std::nullopt;
        }
        level.blocks.resize(static_cast<size_t>(byteCount));
        stream.read(reinterpret_cast<char*>(level.blocks.data()), static_cast<std::streamsize>(byteCount));
        if (!stream)
        {
            return std::nullopt;
        }
    }
    return texture;
}

bool WriteCompressedTexture(const std::filesystem::path& file, const std::string& key, const CompressedTexture& texture)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
    {
        return false;
    }

    const std::filesystem::path temporary = TemporaryPathFor(file);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write(kMagic.data(), kMagic.size());
        WriteValue(stream, kTextureCacheVersion);
        WriteValue(stream, static_cast<uint32_t>(texture.format));
        WriteValue(stream, static_cast<uint32_t>(texture.levels.size()));
        WriteValue(stream, static_cast<uint32_t>(key.size()));
        stream.write(key.data(), static_cast<std::streamsize>(key.size()));
        for (const CompressedTextureLevel& level : texture.levels)
        {
            WriteValue(stream, level.width);
            WriteValue(stream, level.height);
            WriteValue(stream, static_cast<uint64_t>(level.blocks.size()));
            stream.write(reinterpret_cast<const char*>(level.blocks.data()), static_cast<std::streamsize>(level.blocks.size()));
        }
        if (!stream)
        {
            stream.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    // Replaces an existing file in one step, so a concurrent reader sees the old file or the new
    // one and never a mix.
    std::filesystem::rename(temporary, file, error);
    if (error)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

CompressedTextureLoad LoadOrCompressTexture(
    const std::filesystem::path& imagePath,
    TextureUsage usage,
    const std::filesystem::path& cacheDirectory)
{
    const std::string key = BuildCompressedTextureKey(imagePath, usage);
    const std::filesystem::path file = CompressedTextureCacheFile(cacheDirectory, key);
    if (std::optional<CompressedTexture> cached = ReadCompressedTexture(file, key))
    {
        return CompressedTextureLoad{std::move(*cached), true};
    }

    CompressedTextureLoad load{};
    load.texture = CompressTexture(TextureLoader::LoadRGBA8(imagePath.string()), usage);
    if (!WriteCompressedTexture(file, key, load.texture))
    {
        LOG_WARN("Could not cache the compressed form of '{}' at '{}'", imagePath.string(), file.string());
    }
    return load;
}
}
