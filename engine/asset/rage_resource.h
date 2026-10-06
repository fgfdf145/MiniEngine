#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

// A RAGE resource file (the "RSC7" container GTA V keeps its .yft, .ydr and .ytd in): a 16-byte
// header, then one raw-deflate stream holding the resource's system pages followed by its graphics
// pages. Structures inside point at each other with 64-bit addresses: 0x5xxxxxxx is an offset into
// the system pages, 0x6xxxxxxx into the graphics pages, 0 is null. Every read here is bounds-checked
// and throws std::runtime_error, naming the file, rather than reading past the data.
class RageResource
{
  public:
    static constexpr std::uint32_t kMagic = 0x37435352; // "RSC7"
    static constexpr std::uint64_t kSystemBase = 0x50000000;
    static constexpr std::uint64_t kGraphicsBase = 0x60000000;

    static RageResource Load(const std::filesystem::path& path);
    // `file` is the whole file, header included; `source` names it in errors.
    static RageResource FromFile(const std::vector<std::uint8_t>& file, const std::string& source);
    // A resource built from its already inflated pages (for tests).
    static RageResource FromPages(std::uint32_t version, std::vector<std::uint8_t> system, std::vector<std::uint8_t> graphics,
                                  const std::string& source);

    // The resource type's version from the header: 171 for a GTA V Enhanced (gen9) .yft, 5 for its
    // .ytd; the original PC release has 162 and 13.
    std::uint32_t Version() const
    {
        return m_version;
    }
    const std::string& Source() const
    {
        return m_source;
    }
    size_t SystemSize() const
    {
        return m_system.size();
    }
    size_t GraphicsSize() const
    {
        return m_graphics.size();
    }

    // `size` bytes at `address`. Throws for a null address or a range outside its pages.
    const std::uint8_t* Bytes(std::uint64_t address, size_t size) const;

    template <typename T>
    T Read(std::uint64_t address) const
    {
        T value;
        std::memcpy(&value, Bytes(address, sizeof(T)), sizeof(T));
        return value;
    }

    // `count` values of T from `address`; empty for a count of 0 (whatever the address).
    template <typename T>
    std::vector<T> ReadArray(std::uint64_t address, size_t count) const
    {
        std::vector<T> values(count);
        if (count != 0)
        {
            std::memcpy(values.data(), Bytes(address, sizeof(T) * count), sizeof(T) * count);
        }
        return values;
    }

    // The zero-terminated string at `address`; empty for a null address.
    std::string ReadString(std::uint64_t address) const;

    // The page size sum a header's flags word describes (its low 28 bits), in bytes.
    static size_t PagesSize(std::uint32_t flags);

  private:
    std::uint32_t m_version = 0;
    std::vector<std::uint8_t> m_system;
    std::vector<std::uint8_t> m_graphics;
    std::string m_source;
};

// RAGE's name hash (Bob Jenkins' one-at-a-time over the lower-cased text), which GTA V files use for
// shader, parameter and texture names.
std::uint32_t JenkinsHash(std::string_view text);

}
