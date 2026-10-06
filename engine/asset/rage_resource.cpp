#include "rage_resource.h"

#include <stb_image.h>

#include <cctype>
#include <climits>
#include <fstream>
#include <iterator>

namespace me
{

namespace
{
constexpr size_t kHeaderSize = 16;
// Larger than any vehicle or texture dictionary the game ships (a few tens of megabytes).
constexpr size_t kMaxPagesSize = 1024u * 1024u * 1024u;

std::uint32_t ReadU32(const std::uint8_t* bytes)
{
    std::uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
}

size_t RageResource::PagesSize(std::uint32_t flags)
{
    // The page counts of nine sizes (the base page size times 1, 1/2, ... 1/256 of the largest),
    // packed from bit 27 down; the low four bits shift the base page size of 512 bytes.
    const size_t counts = static_cast<size_t>((flags >> 27) & 0x1) * 1 + static_cast<size_t>((flags >> 26) & 0x1) * 2 +
                          static_cast<size_t>((flags >> 25) & 0x1) * 4 + static_cast<size_t>((flags >> 24) & 0x1) * 8 +
                          static_cast<size_t>((flags >> 17) & 0x7F) * 16 + static_cast<size_t>((flags >> 11) & 0x3F) * 32 +
                          static_cast<size_t>((flags >> 7) & 0xF) * 64 + static_cast<size_t>((flags >> 5) & 0x3) * 128 +
                          static_cast<size_t>((flags >> 4) & 0x1) * 256;
    return (static_cast<size_t>(0x200) << (flags & 0xF)) * counts;
}

RageResource RageResource::Load(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return FromFile(file, path.string());
}

RageResource RageResource::FromFile(const std::vector<std::uint8_t>& file, const std::string& source)
{
    if (file.size() < kHeaderSize || ReadU32(file.data()) != kMagic)
    {
        throw std::runtime_error("'" + source + "' is not a RAGE resource (no RSC7 header)");
    }
    const std::uint32_t version = ReadU32(file.data() + 4);
    const size_t systemSize = PagesSize(ReadU32(file.data() + 8));
    const size_t graphicsSize = PagesSize(ReadU32(file.data() + 12));
    const size_t total = systemSize + graphicsSize;
    if (total == 0 || total > kMaxPagesSize || file.size() - kHeaderSize > static_cast<size_t>(INT_MAX))
    {
        throw std::runtime_error("'" + source + "' has an implausible page size of " + std::to_string(total) + " bytes");
    }

    std::vector<std::uint8_t> pages(total);
    const int inflated = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(pages.data()), static_cast<int>(total),
                                                          reinterpret_cast<const char*>(file.data() + kHeaderSize),
                                                          static_cast<int>(file.size() - kHeaderSize));
    if (inflated < 0)
    {
        // An encrypted entry (copied out of an archive without decrypting it) lands here too.
        throw std::runtime_error("'" + source + "' does not inflate: corrupt, or still encrypted");
    }
    std::vector<std::uint8_t> graphics(pages.begin() + static_cast<std::ptrdiff_t>(systemSize), pages.end());
    pages.resize(systemSize);
    return FromPages(version, std::move(pages), std::move(graphics), source);
}

RageResource RageResource::FromPages(std::uint32_t version, std::vector<std::uint8_t> system, std::vector<std::uint8_t> graphics,
                                     const std::string& source)
{
    RageResource resource;
    resource.m_version = version;
    resource.m_system = std::move(system);
    resource.m_graphics = std::move(graphics);
    resource.m_source = source;
    return resource;
}

const std::uint8_t* RageResource::Bytes(std::uint64_t address, size_t size) const
{
    const std::vector<std::uint8_t>* pages = nullptr;
    std::uint64_t offset = 0;
    if ((address & 0xF0000000u) == kSystemBase && (address >> 32) == 0)
    {
        pages = &m_system;
        offset = address & 0x0FFFFFFFu;
    }
    else if ((address & 0xF0000000u) == kGraphicsBase && (address >> 32) == 0)
    {
        pages = &m_graphics;
        offset = address & 0x0FFFFFFFu;
    }
    if (pages == nullptr || offset > pages->size() || size > pages->size() - offset)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        throw std::runtime_error("'" + m_source + "' points outside its data (" + std::to_string(size) + " bytes at " + text + ")");
    }
    return pages->data() + offset;
}

std::string RageResource::ReadString(std::uint64_t address) const
{
    if (address == 0)
    {
        return {};
    }
    const std::uint8_t* start = Bytes(address, 1);
    const std::vector<std::uint8_t>& pages = (address & 0xF0000000u) == kSystemBase ? m_system : m_graphics;
    const std::uint8_t* end = pages.data() + pages.size();
    const std::uint8_t* terminator = static_cast<const std::uint8_t*>(std::memchr(start, 0, static_cast<size_t>(end - start)));
    if (terminator == nullptr)
    {
        throw std::runtime_error("'" + m_source + "' has an unterminated string");
    }
    return std::string(reinterpret_cast<const char*>(start), static_cast<size_t>(terminator - start));
}

std::uint32_t JenkinsHash(std::string_view text)
{
    std::uint32_t hash = 0;
    for (const char character : text)
    {
        hash += static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(character)));
        hash += hash << 10;
        hash ^= hash >> 6;
    }
    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}

}
