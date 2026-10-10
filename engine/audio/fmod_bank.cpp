#include "fmod_bank.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <system_error>

namespace me
{

namespace
{
using Guid = std::array<uint8_t, 16>;
constexpr Guid kNullGuid{};

// The properties an automation can drive (the u32 in CTRL and PROP records).
constexpr uint32_t kPropertyVolume = 0;
constexpr uint32_t kPropertyPitch = 1;
constexpr uint32_t kPropertyFade = 4;

// Timeline positions are counted in 48 kHz frames whatever the samples' own rate.
constexpr float kTimelineRate = 48000.0f;

struct Reader
{
    std::span<const uint8_t> bytes;

    bool Has(size_t offset, size_t size) const
    {
        return offset <= bytes.size() && size <= bytes.size() - offset;
    }
    uint32_t U32(size_t offset) const
    {
        uint32_t value = 0;
        if (Has(offset, 4))
        {
            std::memcpy(&value, bytes.data() + offset, 4);
        }
        return value;
    }
    uint16_t U16(size_t offset) const
    {
        uint16_t value = 0;
        if (Has(offset, 2))
        {
            std::memcpy(&value, bytes.data() + offset, 2);
        }
        return value;
    }
    uint64_t U64(size_t offset) const
    {
        uint64_t value = 0;
        if (Has(offset, 8))
        {
            std::memcpy(&value, bytes.data() + offset, 8);
        }
        return value;
    }
    float F32(size_t offset) const
    {
        float value = 0.0f;
        if (Has(offset, 4))
        {
            std::memcpy(&value, bytes.data() + offset, 4);
        }
        return value;
    }
    Guid Id(size_t offset) const
    {
        Guid id{};
        if (Has(offset, 16))
        {
            std::memcpy(id.data(), bytes.data() + offset, 16);
        }
        return id;
    }
};

// A list inside a record: a 16-bit count stored as count * 2 + 1, then, unless the list is empty, a
// 16-bit element size and the elements. Calls `visit(elementOffset)` for each one that fits and returns
// the offset after the list.
template <typename Visit>
size_t ForEachListEntry(const Reader& reader, size_t offset, Visit&& visit)
{
    if (!reader.Has(offset, 2))
    {
        return offset;
    }
    const uint32_t count = reader.U16(offset) >> 1;
    if (count == 0 || !reader.Has(offset, 4))
    {
        return offset + 2;
    }
    const uint32_t size = reader.U16(offset + 2);
    for (uint32_t index = 0; index < count; ++index)
    {
        const size_t entry = offset + 4 + static_cast<size_t>(index) * size;
        if (!reader.Has(entry, size))
        {
            return reader.bytes.size();
        }
        visit(entry);
    }
    return offset + 4 + static_cast<size_t>(count) * size;
}

// A chunk of the RIFF tree: its tag, and its payload (a LIST's after its list type).
struct Chunk
{
    std::string tag; // "LIST" chunks carry their list type here instead: "LIST:WAIT"
    Reader data;
    std::vector<Chunk> children;
};

void ParseChunks(std::span<const uint8_t> bytes, std::vector<Chunk>& out, int depth)
{
    size_t offset = 0;
    while (offset + 8 <= bytes.size())
    {
        char tag[5] = {};
        std::memcpy(tag, bytes.data() + offset, 4);
        uint32_t size = 0;
        std::memcpy(&size, bytes.data() + offset + 4, 4);
        const size_t payload = offset + 8;
        if (size > bytes.size() - payload)
        {
            return;
        }
        Chunk chunk;
        if ((std::strcmp(tag, "LIST") == 0 || std::strcmp(tag, "RIFF") == 0) && size >= 4 && depth < 16)
        {
            char type[5] = {};
            std::memcpy(type, bytes.data() + payload, 4);
            chunk.tag = std::string("LIST:") + type;
            chunk.data.bytes = bytes.subspan(payload + 4, size - 4);
            ParseChunks(chunk.data.bytes, chunk.children, depth + 1);
        }
        else
        {
            chunk.tag = tag;
            chunk.data.bytes = bytes.subspan(payload, size);
        }
        out.push_back(std::move(chunk));
        // Chunks are padded to an even size.
        offset = payload + size + (size & 1u);
    }
}

const Chunk* Child(const Chunk& parent, std::string_view tag)
{
    for (const Chunk& child : parent.children)
    {
        if (child.tag == tag)
        {
            return &child;
        }
    }
    return nullptr;
}

template <typename Visit>
void ForEachChunk(const std::vector<Chunk>& chunks, Visit&& visit)
{
    for (const Chunk& chunk : chunks)
    {
        visit(chunk);
        ForEachChunk(chunk.children, visit);
    }
}

std::string GuidText(const Guid& id)
{
    // GUIDs are stored with their first three fields little endian (Windows' layout).
    static constexpr std::array<int, 16> kOrder{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (size_t index = 0; index < kOrder.size(); ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
        {
            text += '-';
        }
        const uint8_t byte = id[static_cast<size_t>(kOrder[index])];
        text += kHex[byte >> 4];
        text += kHex[byte & 15];
    }
    return text;
}

struct Parameter
{
    std::string name;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    float seekSpeed = 0.0f;
};

struct Instrument
{
    Guid wave = kNullGuid;
    // A multi instrument's playlist: other instruments' ids.
    std::vector<Guid> playlist;
    float volumeDb = 0.0f;
    float pitchSemitones = 0.0f;
    bool looping = false;
    Guid autopitchParameter = kNullGuid;
    float autopitchReference = 1.0f;
    float autopitchMinimum = 0.0f;
    Guid bus = kNullGuid;
};

struct Bus
{
    Guid parent = kNullGuid;
    float volumeDb = 0.0f;
};

struct Controller
{
    Guid target = kNullGuid;
    Guid parameter = kNullGuid;
    Guid curve = kNullGuid;
    uint32_t property = 0;
};

struct Placement
{
    Guid instrument = kNullGuid;
    Guid sheetParameter = kNullGuid; // null on a timeline
    float start = 0.0f;
    float length = 0.0f;
};

struct Layout
{
    std::map<Guid, uint32_t> waveSamples;
    std::map<Guid, Parameter> parameters;
    std::map<Guid, Instrument> instruments;
    std::map<Guid, Bus> buses;
    std::map<Guid, std::vector<SoundCurvePoint>> curves;
    std::map<Guid, std::vector<std::pair<float, float>>> maps;
    std::vector<std::pair<Guid, Controller>> controllers;
    // The value mapping each controller's property goes through (from the PROP records).
    std::map<Guid, Guid> controllerMaps;
    // Placements by event.
    std::map<Guid, std::vector<Placement>> placements;
    std::set<Guid> loopingTimelines;
    std::map<Guid, Guid> timelineEvents;
    std::vector<Guid> events;
};

// The fields of an instrument record (INST), shared by single and multi instruments.
void ReadInstrumentRecord(const Reader& inst, Instrument& instrument)
{
    instrument.volumeDb = inst.F32(16);
    instrument.pitchSemitones = inst.F32(20);
    instrument.looping = static_cast<int32_t>(inst.U32(24)) == -1;
    instrument.autopitchParameter = inst.Id(53);
    instrument.autopitchReference = inst.F32(69);
    instrument.autopitchMinimum = inst.F32(73);
    instrument.bus = inst.Id(83);
}

void ReadProperties(const Chunk& owner, Layout& layout)
{
    const Chunk* properties = Child(owner, "LIST:PRPS");
    if (properties == nullptr)
    {
        return;
    }
    for (const Chunk& prop : properties->children)
    {
        if (prop.tag != "PROP")
        {
            continue;
        }
        const Guid map = prop.data.Id(8);
        ForEachListEntry(prop.data, 24, [&](size_t entry)
                         {
                             layout.controllerMaps[prop.data.Id(entry)] = map;
                         });
    }
}

void ReadLayout(const std::vector<Chunk>& chunks, Layout& layout)
{
    ForEachChunk(chunks, [&](const Chunk& chunk)
                 {
                     const Reader& data = chunk.data;
                     if (chunk.tag == "WAV ")
                     {
                         layout.waveSamples[data.Id(0)] = data.U32(22);
                     }
                     else if (chunk.tag == "PRMB")
                     {
                         Parameter parameter;
                         const uint16_t nameLength = data.U16(21);
                         if (!data.Has(23, nameLength))
                         {
                             return;
                         }
                         const char* name = reinterpret_cast<const char*>(data.bytes.data() + 23);
                         parameter.name.assign(name, strnlen(name, nameLength));
                         const size_t values = 23 + nameLength;
                         parameter.minimum = data.F32(values);
                         parameter.maximum = data.F32(values + 4);
                         parameter.defaultValue = data.F32(values + 8);
                         parameter.seekSpeed = data.F32(values + 16);
                         layout.parameters[data.Id(0)] = std::move(parameter);
                     }
                     else if (chunk.tag == "LIST:WAIT")
                     {
                         const Chunk* waib = Child(chunk, "WAIB");
                         const Chunk* inst = Child(chunk, "INST");
                         if (waib == nullptr || inst == nullptr)
                         {
                             return;
                         }
                         Instrument instrument;
                         instrument.wave = waib->data.Id(16);
                         ReadInstrumentRecord(inst->data, instrument);
                         layout.instruments[waib->data.Id(0)] = instrument;
                         ReadProperties(chunk, layout);
                     }
                     else if (chunk.tag == "LIST:MUIT")
                     {
                         const Chunk* muib = Child(chunk, "MUIB");
                         const Chunk* playlist = Child(chunk, "PLST");
                         const Chunk* inst = Child(chunk, "INST");
                         if (muib == nullptr || playlist == nullptr || inst == nullptr)
                         {
                             return;
                         }
                         Instrument instrument;
                         // Entries: an instrument's id and its weight.
                         ForEachListEntry(playlist->data, 8, [&](size_t entry)
                                          {
                                              instrument.playlist.push_back(playlist->data.Id(entry));
                                          });
                         ReadInstrumentRecord(inst->data, instrument);
                         layout.instruments[muib->data.Id(0)] = instrument;
                         ReadProperties(chunk, layout);
                     }
                     else if (chunk.tag == "LIST:GBUS" || chunk.tag == "LIST:MBUS")
                     {
                         const Chunk* header = Child(chunk, chunk.tag == "LIST:GBUS" ? "GBSB" : "MBSB");
                         const Chunk* bus = Child(chunk, "BUS ");
                         if (header == nullptr || bus == nullptr || bus->data.bytes.size() < 16)
                         {
                             return;
                         }
                         Bus entry;
                         entry.parent = header->data.Id(18);
                         entry.volumeDb = bus->data.F32(bus->data.bytes.size() - 16);
                         layout.buses[header->data.Id(0)] = entry;
                         ReadProperties(chunk, layout);
                     }
                     else if (chunk.tag == "CURV")
                     {
                         std::vector<SoundCurvePoint> points;
                         ForEachListEntry(data, 32, [&](size_t entry)
                                          {
                                              points.push_back({data.F32(entry), data.F32(entry + 4), data.F32(entry + 8)});
                                          });
                         layout.curves[data.Id(0)] = std::move(points);
                     }
                     else if (chunk.tag == "MAP ")
                     {
                         std::vector<std::pair<float, float>> pairs;
                         ForEachListEntry(data, 16, [&](size_t entry)
                                          {
                                              pairs.emplace_back(data.F32(entry), data.F32(entry + 4));
                                          });
                         layout.maps[data.Id(0)] = std::move(pairs);
                     }
                     else if (chunk.tag == "CTRL")
                     {
                         Controller controller;
                         controller.target = data.Id(16);
                         controller.parameter = data.Id(32);
                         controller.curve = data.Id(48);
                         controller.property = data.U32(64);
                         layout.controllers.emplace_back(data.Id(0), controller);
                     }
                     else if (chunk.tag == "PMLB" && data.bytes.size() > 52)
                     {
                         const Guid parameter = data.Id(0);
                         std::vector<Placement>& placements = layout.placements[data.Id(32)];
                         ForEachListEntry(data, 48, [&](size_t entry)
                                          {
                                              placements.push_back({data.Id(entry), parameter, data.F32(entry + 16), data.F32(entry + 20)});
                                          });
                     }
                     else if (chunk.tag == "LIST:TMLN")
                     {
                         const Chunk* header = Child(chunk, "TLNB");
                         if (header == nullptr)
                         {
                             return;
                         }
                         const Reader& timeline = header->data;
                         const Guid event = timeline.Id(16);
                         layout.timelineEvents[timeline.Id(0)] = event;
                         std::vector<Placement>& placements = layout.placements[event];
                         const auto place = [&](size_t entry)
                         {
                             placements.push_back({timeline.Id(entry), kNullGuid, static_cast<float>(timeline.U32(entry + 16)) / kTimelineRate,
                                                   static_cast<float>(timeline.U32(entry + 20)) / kTimelineRate});
                         };
                         // Two lists of instruments (Kunos' banks use either), then the markers.
                         ForEachListEntry(timeline, ForEachListEntry(timeline, 32, place), place);
                         // A transition region on the timeline (FMOD's loop region) keeps its sounds going.
                         if (const Chunk* transitions = Child(chunk, "LIST:TRNS"); transitions != nullptr && Child(*transitions, "LIST:TRAN") != nullptr)
                         {
                             layout.loopingTimelines.insert(event);
                         }
                     }
                     else if (chunk.tag == "EVTB")
                     {
                         layout.events.push_back(data.Id(0));
                     }
                 });
}

std::string SafeFileName(std::string name)
{
    for (char& c : name)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.'))
        {
            c = '_';
        }
    }
    return name.empty() ? std::string("sample") : name;
}

// The FSB5 sample bank: the samples' headers, names and PCM.
bool ReadFsb5(std::span<const uint8_t> bytes, std::vector<FmodSample>& samples, std::string& error)
{
    const Reader fsb{bytes};
    if (!fsb.Has(0, 60) || std::memcmp(bytes.data(), "FSB5", 4) != 0)
    {
        error = "no FSB5 sample bank";
        return false;
    }
    const uint32_t version = fsb.U32(4);
    const uint32_t count = fsb.U32(8);
    const uint32_t headersSize = fsb.U32(12);
    const uint32_t namesSize = fsb.U32(16);
    const uint32_t dataSize = fsb.U32(20);
    const uint32_t mode = fsb.U32(24);
    // FMOD_SOUND_FORMAT: 1 PCM8, 2 PCM16, 5 PCM float. Compressed formats (Vorbis, ADPCM...) are not read.
    if (mode != 1 && mode != 2 && mode != 5)
    {
        error = "the samples are compressed (FSB5 mode " + std::to_string(mode) + "); only PCM banks are read";
        return false;
    }
    const size_t headers = version == 0 ? 64 : 60;
    const size_t names = headers + headersSize;
    const size_t data = names + namesSize;
    if (!fsb.Has(data, dataSize))
    {
        error = "the FSB5 sample bank is cut short";
        return false;
    }
    static constexpr std::array<uint32_t, 11> kRates{0, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    struct Header
    {
        uint64_t offset = 0;
        uint32_t frames = 0;
        uint32_t channels = 1;
        uint32_t rate = 44100;
    };
    std::vector<Header> entries(count);
    size_t cursor = headers;
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint64_t bits = fsb.U64(cursor);
        cursor += 8;
        Header& header = entries[index];
        const uint32_t rateCode = static_cast<uint32_t>((bits >> 1) & 0xF);
        header.rate = rateCode < kRates.size() ? kRates[rateCode] : 44100;
        header.channels = ((bits >> 5) & 1) + 1;
        header.offset = ((bits >> 6) & 0xFFFFFFF) * 16;
        header.frames = static_cast<uint32_t>((bits >> 34) & 0x3FFFFFFF);
        bool more = (bits & 1) != 0;
        while (more && fsb.Has(cursor, 4))
        {
            const uint32_t extra = fsb.U32(cursor);
            cursor += 4;
            more = (extra & 1) != 0;
            const uint32_t size = (extra >> 1) & 0xFFFFFF;
            const uint32_t type = (extra >> 25) & 0x7F;
            if (type == 1 && size >= 1)
            {
                header.channels = bytes[cursor];
            }
            else if (type == 2 && size >= 4)
            {
                header.rate = fsb.U32(cursor);
            }
            cursor += size;
        }
    }
    const uint32_t bytesPerSample = mode == 1 ? 1 : mode == 2 ? 2
                                                              : 4;
    samples.clear();
    for (uint32_t index = 0; index < count; ++index)
    {
        const Header& header = entries[index];
        FmodSample sample;
        const uint32_t nameOffset = fsb.U32(names + 4 * static_cast<size_t>(index));
        if (namesSize > 0 && nameOffset < namesSize)
        {
            const char* name = reinterpret_cast<const char*>(bytes.data() + names + nameOffset);
            sample.name.assign(name, strnlen(name, namesSize - nameOffset));
        }
        if (sample.name.empty())
        {
            sample.name = "sample_" + std::to_string(index);
        }
        sample.sampleRate = header.rate;
        sample.channels = header.channels;
        const uint64_t end = index + 1 < count ? entries[index + 1].offset : dataSize;
        const uint64_t available = end > header.offset ? end - header.offset : 0;
        const uint64_t wanted = static_cast<uint64_t>(header.frames) * header.channels * bytesPerSample;
        const uint64_t size = std::min(available, wanted);
        const uint8_t* source = bytes.data() + data + header.offset;
        const size_t values = static_cast<size_t>(size / bytesPerSample);
        sample.pcm.resize(values);
        for (size_t value = 0; value < values; ++value)
        {
            if (mode == 2)
            {
                std::memcpy(&sample.pcm[value], source + value * 2, 2);
            }
            else if (mode == 1)
            {
                sample.pcm[value] = static_cast<int16_t>((static_cast<int>(source[value]) - 128) * 256);
            }
            else
            {
                float f = 0.0f;
                std::memcpy(&f, source + value * 4, 4);
                sample.pcm[value] = static_cast<int16_t>(std::clamp(f, -1.0f, 1.0f) * 32767.0f);
            }
        }
        samples.push_back(std::move(sample));
    }
    return true;
}

bool WriteWav(const std::filesystem::path& path, const FmodSample& sample, std::string& error)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        error = "Cannot write '" + path.string() + "'";
        return false;
    }
    const auto put = [&](uint32_t value, int size)
    {
        for (int index = 0; index < size; ++index)
        {
            file.put(static_cast<char>((value >> (8 * index)) & 0xFF));
        }
    };
    const uint32_t dataBytes = static_cast<uint32_t>(sample.pcm.size() * 2);
    file.write("RIFF", 4);
    put(36 + dataBytes, 4);
    file.write("WAVEfmt ", 8);
    put(16, 4);
    put(1, 2);
    put(sample.channels, 2);
    put(sample.sampleRate, 4);
    put(sample.sampleRate * sample.channels * 2, 4);
    put(sample.channels * 2, 2);
    put(16, 2);
    file.write("data", 4);
    put(dataBytes, 4);
    file.write(reinterpret_cast<const char*>(sample.pcm.data()), static_cast<std::streamsize>(dataBytes));
    if (!file.good())
    {
        error = "Cannot write '" + path.string() + "'";
        return false;
    }
    return true;
}

std::string LowerCase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}
}

std::unordered_map<std::string, std::string> ReadFmodGuids(const std::filesystem::path& path)
{
    std::unordered_map<std::string, std::string> names;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line))
    {
        const size_t open = line.find('{');
        const size_t close = line.find('}');
        if (open == std::string::npos || close == std::string::npos || close <= open)
        {
            continue;
        }
        std::string name = line.substr(close + 1);
        name.erase(0, name.find_first_not_of(" \t"));
        while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
        {
            name.pop_back();
        }
        names[LowerCase(line.substr(open + 1, close - open - 1))] = name;
    }
    return names;
}

std::optional<FmodBank> ReadFmodBank(const std::filesystem::path& path, const std::unordered_map<std::string, std::string>& guidNames, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "Cannot open '" + path.string() + "'";
        return std::nullopt;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "FEV ", 4) != 0)
    {
        error = "'" + path.string() + "' is not an FMOD Studio bank";
        return std::nullopt;
    }

    std::vector<Chunk> chunks;
    ParseChunks(std::span<const uint8_t>(bytes), chunks, 0);
    FmodBank result;
    const Chunk* sampleChunk = nullptr;
    ForEachChunk(chunks, [&](const Chunk& chunk)
                 {
                     if (chunk.tag == "SND " && sampleChunk == nullptr)
                     {
                         sampleChunk = &chunk;
                     }
                 });
    if (sampleChunk == nullptr)
    {
        error = "'" + path.string() + "' has no samples";
        return std::nullopt;
    }
    // The FSB5 sits inside SND after alignment padding.
    const std::span<const uint8_t> sound = sampleChunk->data.bytes;
    static constexpr std::array<uint8_t, 4> kFsbMagic{'F', 'S', 'B', '5'};
    const auto fsbStart = std::search(sound.begin(), sound.end(), kFsbMagic.begin(), kFsbMagic.end());
    if (fsbStart == sound.end() || !ReadFsb5(sound.subspan(static_cast<size_t>(fsbStart - sound.begin())), result.samples, error))
    {
        error = "'" + path.string() + "': " + (error.empty() ? std::string("no FSB5 sample bank") : error);
        return std::nullopt;
    }

    Layout layout;
    ReadLayout(chunks, layout);
    std::vector<std::string> sampleFiles;
    std::set<std::string> usedFiles;
    for (const FmodSample& sample : result.samples)
    {
        std::string base = SafeFileName(sample.name);
        std::string name = base;
        for (int suffix = 2; usedFiles.contains(name); ++suffix)
        {
            name = base + "_" + std::to_string(suffix);
        }
        usedFiles.insert(name);
        sampleFiles.push_back("sounds/" + name + ".wav");
    }

    const auto clipOf = [&](const Instrument& instrument, float volumeDb) -> std::optional<SoundClip>
    {
        const auto wave = layout.waveSamples.find(instrument.wave);
        if (wave == layout.waveSamples.end() || wave->second >= sampleFiles.size())
        {
            return std::nullopt;
        }
        return SoundClip{sampleFiles[wave->second], volumeDb};
    };

    for (const Guid& eventId : layout.events)
    {
        SoundEventDesc event;
        const std::string idText = GuidText(eventId);
        const auto named = guidNames.find(idText);
        event.name = named != guidNames.end() ? named->second : idText;
        if (const size_t slash = event.name.rfind('/'); slash != std::string::npos)
        {
            event.name = event.name.substr(slash + 1);
        }
        event.timelineLoops = layout.loopingTimelines.contains(eventId);

        std::map<Guid, int> busIndices;
        std::set<Guid> parameterIds;
        const auto addAutomations = [&](const Guid& target, std::vector<SoundAutomation>& out)
        {
            for (const auto& [controllerId, controller] : layout.controllers)
            {
                if (controller.target != target)
                {
                    continue;
                }
                const auto parameter = layout.parameters.find(controller.parameter);
                const auto curve = layout.curves.find(controller.curve);
                if (parameter == layout.parameters.end() || curve == layout.curves.end())
                {
                    continue;
                }
                SoundAutomation automation;
                if (controller.property == kPropertyVolume)
                {
                    automation.property = SoundProperty::Volume;
                }
                else if (controller.property == kPropertyPitch)
                {
                    automation.property = SoundProperty::Pitch;
                }
                else if (controller.property == kPropertyFade)
                {
                    automation.property = SoundProperty::Fade;
                }
                else
                {
                    continue;
                }
                automation.parameter = parameter->second.name;
                automation.curve = curve->second;
                if (const auto mapId = layout.controllerMaps.find(controllerId); mapId != layout.controllerMaps.end())
                {
                    if (const auto map = layout.maps.find(mapId->second); map != layout.maps.end() && automation.property != SoundProperty::Fade)
                    {
                        automation.mapping = map->second;
                    }
                }
                parameterIds.insert(controller.parameter);
                out.push_back(std::move(automation));
            }
        };
        // A bus and its parents, the master last; returns its index in the event.
        const std::function<int(const Guid&, int)> addBus = [&](const Guid& id, int depth) -> int
        {
            if (id == kNullGuid || depth > 16)
            {
                return -1;
            }
            if (const auto found = busIndices.find(id); found != busIndices.end())
            {
                return found->second;
            }
            const auto bus = layout.buses.find(id);
            if (bus == layout.buses.end())
            {
                return -1;
            }
            const int parent = addBus(bus->second.parent, depth + 1);
            SoundBus out;
            out.name = GuidText(id);
            out.parent = parent;
            out.volumeDb = bus->second.volumeDb;
            addAutomations(id, out.automations);
            event.buses.push_back(std::move(out));
            const int index = static_cast<int>(event.buses.size()) - 1;
            busIndices[id] = index;
            return index;
        };

        for (const Placement& placement : layout.placements[eventId])
        {
            const auto found = layout.instruments.find(placement.instrument);
            if (found == layout.instruments.end())
            {
                continue;
            }
            const Instrument& source = found->second;
            SoundInstrument instrument;
            if (source.playlist.empty())
            {
                if (std::optional<SoundClip> clip = clipOf(source, 0.0f))
                {
                    instrument.clips.push_back(*clip);
                }
            }
            for (const Guid& entry : source.playlist)
            {
                const auto member = layout.instruments.find(entry);
                if (member == layout.instruments.end())
                {
                    continue;
                }
                if (std::optional<SoundClip> clip = clipOf(member->second, member->second.volumeDb))
                {
                    instrument.clips.push_back(*clip);
                }
            }
            if (instrument.clips.empty())
            {
                continue;
            }
            if (placement.sheetParameter != kNullGuid)
            {
                const auto sheet = layout.parameters.find(placement.sheetParameter);
                if (sheet == layout.parameters.end())
                {
                    continue;
                }
                instrument.sheetParameter = sheet->second.name;
                parameterIds.insert(placement.sheetParameter);
            }
            instrument.start = placement.start;
            instrument.length = placement.length;
            instrument.looping = source.looping;
            instrument.volumeDb = source.volumeDb;
            instrument.pitchSemitones = source.pitchSemitones;
            if (const auto autopitch = layout.parameters.find(source.autopitchParameter); autopitch != layout.parameters.end())
            {
                instrument.autopitchParameter = autopitch->second.name;
                instrument.autopitchReference = source.autopitchReference;
                instrument.autopitchMinimum = source.autopitchMinimum;
                parameterIds.insert(source.autopitchParameter);
            }
            instrument.bus = addBus(source.bus, 0);
            addAutomations(placement.instrument, instrument.automations);
            event.instruments.push_back(std::move(instrument));
        }

        std::set<std::string> parameterNames;
        for (const Guid& id : parameterIds)
        {
            const Parameter& parameter = layout.parameters.at(id);
            if (!parameterNames.insert(parameter.name).second)
            {
                continue;
            }
            event.parameters.push_back({parameter.name, parameter.minimum, parameter.maximum, parameter.defaultValue, parameter.seekSpeed});
        }
        if (!event.instruments.empty())
        {
            result.bank.events.push_back(std::move(event));
        }
    }
    std::sort(result.bank.events.begin(), result.bank.events.end(), [](const SoundEventDesc& a, const SoundEventDesc& b)
              {
                  return a.name < b.name;
              });
    return result;
}

bool ImportFmodBank(const std::filesystem::path& fmodBankPath, const std::filesystem::path& guidsPath, const std::filesystem::path& bankPath, std::string& error)
{
    std::optional<FmodBank> bank = ReadFmodBank(fmodBankPath, ReadFmodGuids(guidsPath), error);
    if (!bank)
    {
        return false;
    }
    const std::filesystem::path directory = bankPath.parent_path();
    std::error_code createError;
    std::filesystem::create_directories(directory / "sounds", createError);
    if (createError)
    {
        error = "Cannot create '" + (directory / "sounds").string() + "': " + createError.message();
        return false;
    }
    // Only the samples an event plays are written.
    std::set<std::string> used;
    for (const SoundEventDesc& event : bank->bank.events)
    {
        for (const SoundInstrument& instrument : event.instruments)
        {
            for (const SoundClip& clip : instrument.clips)
            {
                used.insert(clip.file);
            }
        }
    }
    std::set<std::string> written;
    for (const FmodSample& sample : bank->samples)
    {
        // The same names ReadFmodBank gave the clips.
        std::string base = SafeFileName(sample.name);
        std::string name = base;
        for (int suffix = 2; written.contains(name); ++suffix)
        {
            name = base + "_" + std::to_string(suffix);
        }
        written.insert(name);
        const std::string relative = "sounds/" + name + ".wav";
        if (used.contains(relative) && !WriteWav(directory / relative, sample, error))
        {
            return false;
        }
    }
    return SaveSoundBank(bankPath, bank->bank, error);
}

std::filesystem::path FindAcCarSoundBank(const std::filesystem::path& kn5Path)
{
    const std::filesystem::path carDirectory = kn5Path.parent_path();
    std::error_code error;
    const std::filesystem::path named = carDirectory / "sfx" / (carDirectory.filename().string() + ".bank");
    if (std::filesystem::is_regular_file(named, error))
    {
        return named;
    }
    for (const auto& entry : std::filesystem::directory_iterator(carDirectory / "sfx", error))
    {
        if (entry.is_regular_file() && LowerCase(entry.path().extension().string()) == ".bank")
        {
            return entry.path();
        }
    }
    return {};
}

std::filesystem::path FindAcSoundGuids(const std::filesystem::path& kn5Path)
{
    // content/cars/<car>/<car>.kn5 -> content/sfx/GUIDs.txt
    const std::filesystem::path guids = kn5Path.parent_path().parent_path().parent_path() / "sfx" / "GUIDs.txt";
    std::error_code error;
    return std::filesystem::is_regular_file(guids, error) ? guids : std::filesystem::path{};
}

}
