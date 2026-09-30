#include "acd_archive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace me
{

namespace
{
// A header word that marks the newer archive layout, followed by one more word.
constexpr std::int32_t kNewFormatMarker = -1111;
constexpr size_t kMaxEntries = 4096;
constexpr size_t kMaxNameBytes = 260;

// The key's numbers, folded to a byte at the end as the game does.
std::int32_t Wrap(std::int64_t value)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}

std::string ToLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                   });
    return text;
}

bool EndsWith(const std::string& text, const char* suffix)
{
    const std::string tail(suffix);
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// The seven numbers of the key the name decides; the seventh (index 6) is left out.
std::array<int, 8> KeyNumbers(const std::string& name)
{
    std::vector<std::int32_t> o(name.begin(), name.end());
    for (std::int32_t& c : o)
    {
        c &= 0xff;
    }
    const std::int64_t n = static_cast<std::int64_t>(o.size());

    std::int32_t k1 = 0;
    for (std::int32_t c : o)
    {
        k1 = Wrap(std::int64_t{k1} + c);
    }
    std::int32_t k2 = 0;
    for (std::int64_t i = 0; i < n - 1; i += 2)
    {
        k2 = Wrap(std::int64_t{k2} * o[i]);
        k2 = Wrap(std::int64_t{k2} - o[i + 1]);
    }
    std::int32_t k3 = 0;
    for (std::int64_t i = 1; i < n - 3; i += 3)
    {
        k3 = Wrap(std::int64_t{k3} * o[i]);
        k3 = k3 / (o[i + 1] + 0x1b);
        k3 = Wrap(std::int64_t{k3} + (-0x1b - o[i - 1]));
    }
    std::int32_t k4 = 131;
    for (std::int64_t i = 1; i < n; ++i)
    {
        k4 = Wrap(std::int64_t{k4} - o[i]);
    }
    std::int32_t k5 = 66;
    for (std::int64_t i = 1; i < n - 4; i += 4)
    {
        k5 = Wrap((std::int64_t{o[i]} + 15) * k5 * (std::int64_t{o[i - 1]} + 15) + 22);
    }
    std::int32_t k6 = 101;
    for (std::int64_t i = 0; i < n - 2; i += 2)
    {
        k6 = Wrap(std::int64_t{k6} - o[i]);
    }
    const std::int32_t k8 = o.empty() ? 0 : o.back() + 1;
    return {k1 & 0xff, k2 & 0xff, k3 & 0xff, k4 & 0xff, k5 & 0xff, k6 & 0xff, 0, k8 & 0xff};
}

std::int32_t ReadI32(const std::vector<std::uint8_t>& bytes, size_t offset)
{
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24));
}

// The characters an ini or lut file is made of; anything else is what a wrong key decrypts to.
const std::array<bool, 256>& TextTable()
{
    static const std::array<bool, 256> table = []
    {
        std::array<bool, 256> result{};
        for (unsigned char c : std::string("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 =[]_.;-\r\n,/:\t()|"))
        {
            result[c] = true;
        }
        return result;
    }();
    return table;
}

std::string Decrypt(const std::vector<std::uint8_t>& bytes, const std::string& key)
{
    std::string text(bytes.size(), '\0');
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        text[index] = static_cast<char>(static_cast<std::uint8_t>(bytes[index] - static_cast<std::uint8_t>(key[index % key.size()])));
    }
    return text;
}

// How much of the archive's text files decrypt to text under the key.
double TextScore(const std::vector<AcdArchive::RawEntry>& entries, const std::string& key)
{
    const std::array<bool, 256>& table = TextTable();
    size_t good = 0;
    size_t total = 0;
    for (const AcdArchive::RawEntry& entry : entries)
    {
        if (!EndsWith(entry.name, ".ini") && !EndsWith(entry.name, ".lut"))
        {
            continue;
        }
        for (size_t index = 0; index < entry.bytes.size(); ++index)
        {
            const std::uint8_t plain = static_cast<std::uint8_t>(entry.bytes[index] - static_cast<std::uint8_t>(key[index % key.size()]));
            good += table[plain] ? 1 : 0;
        }
        total += entry.bytes.size();
    }
    return total == 0 ? 0.0 : static_cast<double>(good) / static_cast<double>(total);
}

// The seventh number, or -1 when no candidate stands out.
int FindSeventh(const std::vector<AcdArchive::RawEntry>& entries, const std::string& folderName)
{
    // Text below this length is too little to tell a key from a near miss.
    constexpr double kMinimumScore = 0.97;
    constexpr double kMinimumMargin = 0.002;
    double best = 0.0;
    double second = 0.0;
    int bestSeventh = -1;
    for (int seventh = 0; seventh < 256; ++seventh)
    {
        const double score = TextScore(entries, AcdArchive::MakeKey(folderName, seventh));
        if (score > best)
        {
            second = best;
            best = score;
            bestSeventh = seventh;
        }
        else if (score > second)
        {
            second = score;
        }
    }
    return best >= kMinimumScore && best - second >= kMinimumMargin ? bestSeventh : -1;
}
}

namespace AcdArchive
{
std::string MakeKey(const std::string& folderName, int seventh)
{
    std::array<int, 8> numbers = KeyNumbers(folderName);
    numbers[6] = seventh & 0xff;
    std::string key;
    for (size_t index = 0; index < numbers.size(); ++index)
    {
        key += (index == 0 ? "" : "-") + std::to_string(numbers[index]);
    }
    return key;
}

std::vector<RawEntry> Split(const std::vector<std::uint8_t>& archive, const std::string& source)
{
    const auto corrupt = [&](const std::string& why)
    {
        return std::runtime_error("'" + source + "' is not a readable data.acd: " + why);
    };

    size_t offset = 0;
    if (archive.size() >= 8 && ReadI32(archive, 0) == kNewFormatMarker)
    {
        offset = 8;
    }
    std::vector<RawEntry> entries;
    while (offset < archive.size())
    {
        if (entries.size() >= kMaxEntries || offset + 4 > archive.size())
        {
            throw corrupt("the entry table runs out");
        }
        const std::int32_t nameLength = ReadI32(archive, offset);
        offset += 4;
        if (nameLength <= 0 || static_cast<size_t>(nameLength) > kMaxNameBytes || offset + static_cast<size_t>(nameLength) + 4 > archive.size())
        {
            throw corrupt("an entry name is out of range");
        }
        RawEntry entry;
        entry.name = ToLowerAscii(std::string(archive.begin() + static_cast<std::ptrdiff_t>(offset), archive.begin() + static_cast<std::ptrdiff_t>(offset) + nameLength));
        offset += static_cast<size_t>(nameLength);
        const std::int32_t size = ReadI32(archive, offset);
        offset += 4;
        if (size < 0 || static_cast<size_t>(size) > (archive.size() - offset) / 4)
        {
            throw corrupt("entry '" + entry.name + "' is longer than the file");
        }
        entry.bytes.resize(static_cast<size_t>(size));
        for (size_t index = 0; index < entry.bytes.size(); ++index)
        {
            // One byte to a word; the high bytes are the key's overflow and carry nothing.
            entry.bytes[index] = archive[offset + index * 4];
        }
        offset += static_cast<size_t>(size) * 4;
        entries.push_back(std::move(entry));
    }
    if (entries.empty())
    {
        throw corrupt("it holds no files");
    }
    return entries;
}

Files Parse(const std::vector<std::uint8_t>& archive, const std::string& folderName, const std::string& source)
{
    const std::vector<RawEntry> entries = Split(archive, source);
    std::vector<std::string> spellings{folderName};
    if (ToLowerAscii(folderName) != folderName)
    {
        spellings.push_back(ToLowerAscii(folderName));
    }
    for (const std::string& spelling : spellings)
    {
        const int seventh = FindSeventh(entries, spelling);
        if (seventh < 0)
        {
            continue;
        }
        const std::string key = MakeKey(spelling, seventh);
        Files files;
        for (const RawEntry& entry : entries)
        {
            files[entry.name] = Decrypt(entry.bytes, key);
        }
        return files;
    }
    throw std::runtime_error(
        "Cannot decrypt '" + source + "': no key derived from the folder name '" + folderName +
        "' turns its files into text (the game keys the archive on the folder's name, so a renamed folder does not open)");
}

Files Load(const std::filesystem::path& archivePath)
{
    std::ifstream file(archivePath, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + archivePath.string() + "'");
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return Parse(bytes, archivePath.parent_path().filename().string(), archivePath.string());
}
}
}
