#include "gta5_game_data.h"

#include <engine/core/log/log.h>
#include <engine/core/text/ascii.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace me
{

namespace
{
// carcols.ymt's colour list, which an extraction of the game's text files does not carry: the values
// are DurtyFree/gta-v-data-dumps' vehicleColors.json (PrimarySecondaryColors), the finish its
// MetallicId (normal, 1, 2: metallic; 3 matte; 4 util; 5 worn; 6 brushed; 7 chrome; 8 gold; 9 satin).
constexpr Gta5PaletteColor kPalette[] = {
    {0x080808, Gta5PaintFinish::Metallic, "Metallic Black"}, // 0
    {0x0F0F0F, Gta5PaintFinish::Metallic, "Metallic Graphite Black"}, // 1
    {0x1C1E21, Gta5PaintFinish::Metallic, "Metallic Black Steal"}, // 2
    {0x292C2E, Gta5PaintFinish::Metallic, "Metallic Dark Silver"}, // 3
    {0x5A5E66, Gta5PaintFinish::Metallic, "Metallic Silver"}, // 4
    {0x777C87, Gta5PaintFinish::Metallic, "Metallic Blue Silver"}, // 5
    {0x515459, Gta5PaintFinish::Metallic, "Metallic Steel Gray"}, // 6
    {0x323B47, Gta5PaintFinish::Metallic, "Metallic Shadow Silver"}, // 7
    {0x333333, Gta5PaintFinish::Metallic, "Metallic Stone Silver"}, // 8
    {0x1F2226, Gta5PaintFinish::Metallic, "Metallic Midnight Silver"}, // 9
    {0x23292E, Gta5PaintFinish::Metallic, "Metallic Gun Metal"}, // 10
    {0x121110, Gta5PaintFinish::Metallic, "Metallic Anthracite Grey"}, // 11
    {0x050505, Gta5PaintFinish::Matte, "Matte Black"}, // 12
    {0x121212, Gta5PaintFinish::Matte, "Matte Gray"}, // 13
    {0x2F3233, Gta5PaintFinish::Matte, "Matte Light Grey"}, // 14
    {0x080808, Gta5PaintFinish::Util, "Util Black"}, // 15
    {0x121212, Gta5PaintFinish::Util, "Util Black Poly"}, // 16
    {0x202224, Gta5PaintFinish::Util, "Util Dark silver"}, // 17
    {0x575961, Gta5PaintFinish::Util, "Util Silver"}, // 18
    {0x23292E, Gta5PaintFinish::Util, "Util Gun Metal"}, // 19
    {0x323B47, Gta5PaintFinish::Util, "Util Shadow Silver"}, // 20
    {0x0F1012, Gta5PaintFinish::Worn, "Worn Black"}, // 21
    {0x212121, Gta5PaintFinish::Worn, "Worn Graphite"}, // 22
    {0x5B5D5E, Gta5PaintFinish::Worn, "Worn Silver Grey"}, // 23
    {0x888A99, Gta5PaintFinish::Worn, "Worn Silver"}, // 24
    {0x697187, Gta5PaintFinish::Worn, "Worn Blue Silver"}, // 25
    {0x3B4654, Gta5PaintFinish::Worn, "Worn Shadow Silver"}, // 26
    {0x690000, Gta5PaintFinish::Metallic, "Metallic Red"}, // 27
    {0x8A0B00, Gta5PaintFinish::Metallic, "Metallic Torino Red"}, // 28
    {0x6B0000, Gta5PaintFinish::Metallic, "Metallic Formula Red"}, // 29
    {0x611009, Gta5PaintFinish::Metallic, "Metallic Blaze Red"}, // 30
    {0x4A0A0A, Gta5PaintFinish::Metallic, "Metallic Graceful Red"}, // 31
    {0x470E0E, Gta5PaintFinish::Metallic, "Metallic Garnet Red"}, // 32
    {0x380C00, Gta5PaintFinish::Metallic, "Metallic Desert Red"}, // 33
    {0x26030B, Gta5PaintFinish::Metallic, "Metallic Cabernet Red"}, // 34
    {0x630012, Gta5PaintFinish::Metallic, "Metallic Candy Red"}, // 35
    {0x802800, Gta5PaintFinish::Metallic, "Metallic Sunrise Orange"}, // 36
    {0x6E4F2D, Gta5PaintFinish::Metallic, "Metallic Classic Gold"}, // 37
    {0xBD4800, Gta5PaintFinish::Metallic, "Metallic Orange"}, // 38
    {0x780000, Gta5PaintFinish::Matte, "Matte Red"}, // 39
    {0x360000, Gta5PaintFinish::Matte, "Matte Dark Red"}, // 40
    {0xAB3F00, Gta5PaintFinish::Matte, "Matte Orange"}, // 41
    {0xDE7E00, Gta5PaintFinish::Matte, "Matte Yellow"}, // 42
    {0x520000, Gta5PaintFinish::Util, "Util Red"}, // 43
    {0x8C0404, Gta5PaintFinish::Util, "Util Bright Red"}, // 44
    {0x4A1000, Gta5PaintFinish::Metallic, "Util Garnet Red"}, // 45
    {0x592525, Gta5PaintFinish::Worn, "Worn Red"}, // 46
    {0x754231, Gta5PaintFinish::Worn, "Worn Golden Red"}, // 47
    {0x210804, Gta5PaintFinish::Worn, "Worn Dark Red"}, // 48
    {0x001207, Gta5PaintFinish::Metallic, "Metallic Dark Green"}, // 49
    {0x001A0B, Gta5PaintFinish::Metallic, "Metallic Racing Green"}, // 50
    {0x00211E, Gta5PaintFinish::Metallic, "Metallic Sea Green"}, // 51
    {0x1F261E, Gta5PaintFinish::Metallic, "Metallic Olive Green"}, // 52
    {0x003805, Gta5PaintFinish::Metallic, "Metallic Green"}, // 53
    {0x0B4145, Gta5PaintFinish::Metallic, "Metallic Gasoline Blue Green"}, // 54
    {0x418503, Gta5PaintFinish::Matte, "Matte Lime Green"}, // 55
    {0x0F1F15, Gta5PaintFinish::Util, "Util Dark Green"}, // 56
    {0x023613, Gta5PaintFinish::Util, "Util Green"}, // 57
    {0x162419, Gta5PaintFinish::Worn, "Worn Dark Green"}, // 58
    {0x2A3625, Gta5PaintFinish::Worn, "Worn Green"}, // 59
    {0x455C56, Gta5PaintFinish::Worn, "Worn Sea Wash"}, // 60
    {0x000D14, Gta5PaintFinish::Metallic, "Metallic Midnight Blue"}, // 61
    {0x001029, Gta5PaintFinish::Metallic, "Metallic Dark Blue"}, // 62
    {0x1C2F4F, Gta5PaintFinish::Metallic, "Metallic Saxony Blue"}, // 63
    {0x001B57, Gta5PaintFinish::Metallic, "Metallic Blue"}, // 64
    {0x3B4E78, Gta5PaintFinish::Metallic, "Metallic Mariner Blue"}, // 65
    {0x272D3B, Gta5PaintFinish::Metallic, "Metallic Harbor Blue"}, // 66
    {0x95B2DB, Gta5PaintFinish::Metallic, "Metallic Diamond Blue"}, // 67
    {0x3E627A, Gta5PaintFinish::Metallic, "Metallic Surf Blue"}, // 68
    {0x1C3140, Gta5PaintFinish::Metallic, "Metallic Nautical Blue"}, // 69
    {0x0055C4, Gta5PaintFinish::Metallic, "Metallic Bright Blue"}, // 70
    {0x1A182E, Gta5PaintFinish::Metallic, "Metallic Purple Blue"}, // 71
    {0x161629, Gta5PaintFinish::Metallic, "Metallic Spinnaker Blue"}, // 72
    {0x0E316D, Gta5PaintFinish::Metallic, "Metallic Ultra Blue"}, // 73
    {0x395A83, Gta5PaintFinish::Metallic, "Metallic Bright Blue"}, // 74
    {0x09142E, Gta5PaintFinish::Util, "Util Dark Blue"}, // 75
    {0x0F1021, Gta5PaintFinish::Util, "Util Midnight Blue"}, // 76
    {0x152A52, Gta5PaintFinish::Util, "Util Blue"}, // 77
    {0x324654, Gta5PaintFinish::Util, "Util Sea Foam Blue"}, // 78
    {0x152563, Gta5PaintFinish::Util, "Uil Lightning blue"}, // 79
    {0x223BA1, Gta5PaintFinish::Util, "Util Maui Blue Poly"}, // 80
    {0x1F1FA1, Gta5PaintFinish::Util, "Util Bright Blue"}, // 81
    {0x030E2E, Gta5PaintFinish::Matte, "Matte Dark Blue"}, // 82
    {0x0F1E73, Gta5PaintFinish::Matte, "Matte Blue"}, // 83
    {0x001C32, Gta5PaintFinish::Matte, "Matte Midnight Blue"}, // 84
    {0x2A3754, Gta5PaintFinish::Worn, "Worn Dark blue"}, // 85
    {0x303C5E, Gta5PaintFinish::Worn, "Worn Blue"}, // 86
    {0x3B6796, Gta5PaintFinish::Worn, "Worn Light blue"}, // 87
    {0xF5890F, Gta5PaintFinish::Metallic, "Metallic Taxi Yellow"}, // 88
    {0xD9A600, Gta5PaintFinish::Metallic, "Metallic Race Yellow"}, // 89
    {0x4A341B, Gta5PaintFinish::Metallic, "Metallic Bronze"}, // 90
    {0xA2A827, Gta5PaintFinish::Metallic, "Metallic Yellow Bird"}, // 91
    {0x568F00, Gta5PaintFinish::Metallic, "Metallic Lime"}, // 92
    {0x57514B, Gta5PaintFinish::Metallic, "Metallic Champagne"}, // 93
    {0x291B06, Gta5PaintFinish::Metallic, "Metallic Pueblo Beige"}, // 94
    {0x262117, Gta5PaintFinish::Metallic, "Metallic Dark Ivory"}, // 95
    {0x120D07, Gta5PaintFinish::Metallic, "Metallic Choco Brown"}, // 96
    {0x332111, Gta5PaintFinish::Metallic, "Metallic Golden Brown"}, // 97
    {0x3D3023, Gta5PaintFinish::Metallic, "Metallic Light Brown"}, // 98
    {0x5E5343, Gta5PaintFinish::Metallic, "Metallic Straw Beige"}, // 99
    {0x37382B, Gta5PaintFinish::Metallic, "Metallic Moss Brown"}, // 100
    {0x221918, Gta5PaintFinish::Metallic, "Metallic Biston Brown"}, // 101
    {0x575036, Gta5PaintFinish::Metallic, "Metallic Beechwood"}, // 102
    {0x241309, Gta5PaintFinish::Metallic, "Metallic Dark Beechwood"}, // 103
    {0x3B1700, Gta5PaintFinish::Metallic, "Metallic Choco Orange"}, // 104
    {0x6E6246, Gta5PaintFinish::Metallic, "Metallic Beach Sand"}, // 105
    {0x998D73, Gta5PaintFinish::Metallic, "Metallic Sun Bleeched Sand"}, // 106
    {0xCFC0A5, Gta5PaintFinish::Metallic, "Metallic Cream"}, // 107
    {0x1F1709, Gta5PaintFinish::Util, "Util Brown"}, // 108
    {0x3D311D, Gta5PaintFinish::Util, "Util Medium Brown"}, // 109
    {0x665847, Gta5PaintFinish::Util, "Util Light Brown"}, // 110
    {0xF0F0F0, Gta5PaintFinish::Metallic, "Metallic White"}, // 111
    {0xB3B9C9, Gta5PaintFinish::Metallic, "Metallic Frost White"}, // 112
    {0x615F55, Gta5PaintFinish::Worn, "Worn Honey Beige"}, // 113
    {0x241E1A, Gta5PaintFinish::Worn, "Worn Brown"}, // 114
    {0x171413, Gta5PaintFinish::Worn, "Worn Dark Brown"}, // 115
    {0x3B372F, Gta5PaintFinish::Worn, "Worn straw beige"}, // 116
    {0x3B4045, Gta5PaintFinish::Brushed, "Brushed Steel"}, // 117
    {0x1A1E21, Gta5PaintFinish::Brushed, "Brushed Black steel"}, // 118
    {0x5E646B, Gta5PaintFinish::Brushed, "Brushed Aluminium"}, // 119
    {0x000000, Gta5PaintFinish::Chrome, "Chrome"}, // 120
    {0xB0B0B0, Gta5PaintFinish::Worn, "Worn Off White"}, // 121
    {0x999999, Gta5PaintFinish::Util, "Util Off White"}, // 122
    {0xB56519, Gta5PaintFinish::Worn, "Worn Orange"}, // 123
    {0xC45C33, Gta5PaintFinish::Worn, "Worn Light Orange"}, // 124
    {0x47783C, Gta5PaintFinish::Metallic, "Metallic Securicor Green"}, // 125
    {0xBA8425, Gta5PaintFinish::Worn, "Worn Taxi Yellow"}, // 126
    {0x2A77A1, Gta5PaintFinish::Metallic, "police car blue"}, // 127
    {0x243022, Gta5PaintFinish::Matte, "Matte Green"}, // 128
    {0x6B5F54, Gta5PaintFinish::Matte, "Matte Brown"}, // 129
    {0xC96E34, Gta5PaintFinish::Worn, "Worn Orange"}, // 130
    {0xD9D9D9, Gta5PaintFinish::Matte, "Matte White"}, // 131
    {0xF0F0F0, Gta5PaintFinish::Worn, "Worn White"}, // 132
    {0x3F4228, Gta5PaintFinish::Matte, "Worn Olive Army Green"}, // 133
    {0xFFFFFF, Gta5PaintFinish::Metallic, "Pure White"}, // 134
    {0xB01259, Gta5PaintFinish::Metallic, "Hot Pink"}, // 135
    {0xF69799, Gta5PaintFinish::Metallic, "Salmon pink"}, // 136
    {0x8F2F55, Gta5PaintFinish::Metallic, "Metallic Vermillion Pink"}, // 137
    {0xC26610, Gta5PaintFinish::Metallic, "Orange"}, // 138
    {0x69BD45, Gta5PaintFinish::Metallic, "Green"}, // 139
    {0x00AEEF, Gta5PaintFinish::Metallic, "Blue"}, // 140
    {0x000108, Gta5PaintFinish::Metallic, "Mettalic Black Blue"}, // 141
    {0x050008, Gta5PaintFinish::Metallic, "Metallic Black Purple"}, // 142
    {0x080000, Gta5PaintFinish::Metallic, "Metallic Black Red"}, // 143
    {0x565751, Gta5PaintFinish::Metallic, "hunter green"}, // 144
    {0x320642, Gta5PaintFinish::Metallic, "Metallic Purple"}, // 145
    {0x00080F, Gta5PaintFinish::Metallic, "Metaillic V Dark Blue"}, // 146
    {0x080808, Gta5PaintFinish::Metallic, "MODSHOP BLACK1"}, // 147
    {0x320642, Gta5PaintFinish::Matte, "Matte Purple"}, // 148
    {0x050008, Gta5PaintFinish::Matte, "Matte Dark Purple"}, // 149
    {0x6B0B00, Gta5PaintFinish::Metallic, "Metallic Lava Red"}, // 150
    {0x121710, Gta5PaintFinish::Matte, "Matte Forest Green"}, // 151
    {0x323325, Gta5PaintFinish::Matte, "Matte Olive Drab"}, // 152
    {0x3B352D, Gta5PaintFinish::Matte, "Matte Desert Brown"}, // 153
    {0x706656, Gta5PaintFinish::Matte, "Matte Desert Tan"}, // 154
    {0x2B302B, Gta5PaintFinish::Matte, "Matte Foilage Green"}, // 155
    {0x414347, Gta5PaintFinish::Metallic, "DEFAULT ALLOY COLOR"}, // 156
    {0x6690B5, Gta5PaintFinish::Metallic, "Epsilon Blue"}, // 157
    {0x47391B, Gta5PaintFinish::Gold, "MP100 GOLD"}, // 158
    {0x47391B, Gta5PaintFinish::Satin, "MP100 GOLD SATIN"}, // 159
    {0xFFD859, Gta5PaintFinish::Gold, "MP100 GOLD SPEC"}, // 160
};

float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool IsSpace(char character)
{
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string Trim(std::string_view text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsSpace(text[begin]))
    {
        ++begin;
    }
    while (end > begin && IsSpace(text[end - 1]))
    {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

class XmlParser
{
  public:
    explicit XmlParser(std::string_view text) : m_text(text)
    {
    }

    Gta5GameDataRules::XmlElement ParseDocument()
    {
        SkipMisc();
        if (m_position >= m_text.size() || m_text[m_position] != '<')
        {
            Fail("no root element");
        }
        return ParseElement();
    }

  private:
    [[noreturn]] void Fail(const std::string& what) const
    {
        throw std::runtime_error("malformed XML at offset " + std::to_string(m_position) + ": " + what);
    }

    bool StartsWith(std::string_view prefix) const
    {
        return m_text.substr(m_position, prefix.size()) == prefix;
    }

    void SkipUntil(std::string_view terminator)
    {
        const size_t end = m_text.find(terminator, m_position);
        if (end == std::string_view::npos)
        {
            Fail("unterminated '" + std::string(terminator) + "'");
        }
        m_position = end + terminator.size();
    }

    // Whitespace, a byte order mark, the declaration, comments and a doctype before the root.
    void SkipMisc()
    {
        while (m_position < m_text.size())
        {
            const unsigned char character = static_cast<unsigned char>(m_text[m_position]);
            if (IsSpace(m_text[m_position]) || character == 0xEF || character == 0xBB || character == 0xBF)
            {
                ++m_position;
            }
            else if (StartsWith("<?"))
            {
                SkipUntil("?>");
            }
            else if (StartsWith("<!--"))
            {
                SkipUntil("-->");
            }
            else if (StartsWith("<!"))
            {
                SkipUntil(">");
            }
            else
            {
                return;
            }
        }
    }

    void SkipSpaces()
    {
        while (m_position < m_text.size() && IsSpace(m_text[m_position]))
        {
            ++m_position;
        }
    }

    std::string ParseName()
    {
        const size_t start = m_position;
        while (m_position < m_text.size())
        {
            const char character = m_text[m_position];
            if (IsSpace(character) || character == '>' || character == '/' || character == '=')
            {
                break;
            }
            ++m_position;
        }
        if (m_position == start)
        {
            Fail("expected a name");
        }
        return std::string(m_text.substr(start, m_position - start));
    }

    Gta5GameDataRules::XmlElement ParseElement()
    {
        Gta5GameDataRules::XmlElement element;
        ++m_position; // '<'
        element.name = ParseName();
        for (;;)
        {
            SkipSpaces();
            if (m_position >= m_text.size())
            {
                Fail("unterminated tag <" + element.name + ">");
            }
            if (StartsWith("/>"))
            {
                m_position += 2;
                return element;
            }
            if (m_text[m_position] == '>')
            {
                ++m_position;
                break;
            }
            const std::string attribute = ParseName();
            SkipSpaces();
            if (m_position >= m_text.size() || m_text[m_position] != '=')
            {
                Fail("expected '=' after " + attribute);
            }
            ++m_position;
            SkipSpaces();
            const char quote = m_position < m_text.size() ? m_text[m_position] : '\0';
            if (quote != '"' && quote != '\'')
            {
                Fail("expected a quoted value");
            }
            const size_t end = m_text.find(quote, m_position + 1);
            if (end == std::string_view::npos)
            {
                Fail("unterminated attribute value");
            }
            element.attributes[attribute] = std::string(m_text.substr(m_position + 1, end - m_position - 1));
            m_position = end + 1;
        }

        std::string text;
        for (;;)
        {
            if (m_position >= m_text.size())
            {
                Fail("unterminated element <" + element.name + ">");
            }
            if (StartsWith("</"))
            {
                m_position += 2;
                const std::string name = ParseName();
                if (name != element.name)
                {
                    Fail("</" + name + "> closes <" + element.name + ">");
                }
                SkipSpaces();
                if (m_position >= m_text.size() || m_text[m_position] != '>')
                {
                    Fail("expected '>'");
                }
                ++m_position;
                break;
            }
            if (StartsWith("<!--"))
            {
                SkipUntil("-->");
            }
            else if (StartsWith("<![CDATA["))
            {
                const size_t start = m_position + 9;
                SkipUntil("]]>");
                text.append(m_text.substr(start, m_position - 3 - start));
            }
            else if (m_text[m_position] == '<')
            {
                element.children.push_back(ParseElement());
            }
            else
            {
                const size_t next = m_text.find('<', m_position);
                const size_t stop = next == std::string_view::npos ? m_text.size() : next;
                text.append(m_text.substr(m_position, stop - m_position));
                m_position = stop;
            }
        }
        element.text = Trim(text);
        return element;
    }

    std::string_view m_text;
    size_t m_position = 0;
};

std::optional<float> ParseFloat(const std::string& text)
{
    try
    {
        const float value = std::stof(text);
        return std::isfinite(value) ? std::optional<float>(value) : std::nullopt;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// The part of a path below the extraction root that tells packs apart: "base" or "dlc/<pack>".
std::string PackOf(const std::filesystem::path& relative)
{
    auto it = relative.begin();
    if (it == relative.end())
    {
        return {};
    }
    std::string pack = ToLowerAscii(it->string());
    if ((pack == "dlc" || pack == "dlc_patch") && ++it != relative.end())
    {
        pack += "/" + ToLowerAscii(it->string());
    }
    return pack;
}

Gta5Handling ReadHandling(const Gta5GameDataRules::XmlElement& item)
{
    Gta5Handling handling;
    handling.name = item.ChildText("handlingName");
    handling.modelFlags = item.ChildText("strModelFlags");
    handling.handlingFlags = item.ChildText("strHandlingFlags");
    for (const Gta5GameDataRules::XmlElement& child : item.children)
    {
        const auto value = child.attributes.find("value");
        if (value != child.attributes.end())
        {
            if (const std::optional<float> number = ParseFloat(value->second))
            {
                handling.values[child.name] = *number;
            }
            continue;
        }
        const auto x = child.attributes.find("x");
        const auto y = child.attributes.find("y");
        const auto z = child.attributes.find("z");
        if (x != child.attributes.end() && y != child.attributes.end() && z != child.attributes.end())
        {
            handling.vectors[child.name] = glm::vec3(ParseFloat(x->second).value_or(0.0f), ParseFloat(y->second).value_or(0.0f),
                                                     ParseFloat(z->second).value_or(0.0f));
        }
    }
    return handling;
}

std::mutex g_cacheMutex;
std::map<std::filesystem::path, std::shared_ptr<const Gta5GameData>> g_cache;
}

std::optional<Gta5PaletteColor> Gta5PaletteEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(std::size(kPalette)))
    {
        return std::nullopt;
    }
    return kPalette[index];
}

glm::vec3 Gta5PaletteLinear(int index)
{
    const std::uint32_t rgb = Gta5PaletteEntry(index).value_or(kPalette[0]).rgb;
    return {SrgbToLinear(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f), SrgbToLinear(static_cast<float>((rgb >> 8) & 0xFF) / 255.0f),
            SrgbToLinear(static_cast<float>(rgb & 0xFF) / 255.0f)};
}

float Gta5Handling::Value(const std::string& key, float fallback) const
{
    const auto found = values.find(key);
    return found != values.end() ? found->second : fallback;
}

namespace Gta5GameDataRules
{
const XmlElement* XmlElement::Child(std::string_view childName) const
{
    for (const XmlElement& child : children)
    {
        if (child.name == childName)
        {
            return &child;
        }
    }
    return nullptr;
}

std::string XmlElement::ChildText(std::string_view childName) const
{
    const XmlElement* child = Child(childName);
    return child != nullptr ? child->text : std::string{};
}

std::optional<float> XmlElement::ChildValue(std::string_view childName) const
{
    const XmlElement* child = Child(childName);
    if (child == nullptr)
    {
        return std::nullopt;
    }
    const auto value = child->attributes.find("value");
    return value != child->attributes.end() ? ParseFloat(value->second) : std::nullopt;
}

XmlElement ParseXml(std::string_view text)
{
    return XmlParser(text).ParseDocument();
}

int MetaPriority(const std::filesystem::path& relativePath)
{
    const std::string path = ToLowerAscii(relativePath.generic_string());
    if (path.find("/update/dlc_patch/") != std::string::npos || path.starts_with("dlc_patch/"))
    {
        return 4;
    }
    if (path.find("/update/") != std::string::npos)
    {
        return 3;
    }
    if (path.starts_with("dlc/"))
    {
        // The packs that patch earlier ones ("patch2023_01", "patchday27ng") come after them.
        return path.starts_with("dlc/patch") ? 2 : 1;
    }
    return 0;
}
}

std::shared_ptr<const Gta5GameData> Gta5GameData::ForFile(const std::filesystem::path& anyFile)
{
    const std::filesystem::path absolute = std::filesystem::absolute(anyFile).lexically_normal();
    std::filesystem::path root = absolute.parent_path();
    for (std::filesystem::path folder = absolute.parent_path(); !folder.empty(); folder = folder.parent_path())
    {
        std::error_code error;
        if (std::filesystem::is_directory(folder / "base", error) || std::filesystem::exists(folder / "manifest.tsv", error))
        {
            root = folder;
            break;
        }
        if (folder == folder.parent_path())
        {
            break;
        }
    }

    const std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (const auto cached = g_cache.find(root); cached != g_cache.end())
    {
        return cached->second;
    }

    auto data = std::make_shared<Gta5GameData>();
    data->m_root = root;
    struct Meta
    {
        std::filesystem::path path;
        int priority = 0;
    };
    std::vector<Meta> vehicleMetas;
    std::vector<Meta> variationMetas;
    std::vector<Meta> handlingMetas;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        if (!it->is_regular_file(error))
        {
            continue;
        }
        const std::filesystem::path& path = it->path();
        const std::string file = ToLowerAscii(path.filename().string());
        if (file.ends_with(".ytd"))
        {
            data->m_dictionaries[ToLowerAscii(path.stem().string())].push_back(path);
            continue;
        }
        const Meta meta{path, Gta5GameDataRules::MetaPriority(path.lexically_relative(root))};
        if (file == "vehicles.meta")
        {
            vehicleMetas.push_back(meta);
        }
        else if (file == "carvariations.meta")
        {
            variationMetas.push_back(meta);
        }
        else if (file == "handling.meta")
        {
            handlingMetas.push_back(meta);
        }
    }
    // Lowest priority first, so that a later file replaces an earlier one's entries.
    const auto byPriority = [](std::vector<Meta>& metas)
    {
        std::sort(metas.begin(), metas.end(),
                  [](const Meta& a, const Meta& b) { return a.priority != b.priority ? a.priority < b.priority : a.path < b.path; });
    };
    byPriority(vehicleMetas);
    byPriority(variationMetas);
    byPriority(handlingMetas);

    const auto parse = [](const Meta& meta) -> std::optional<Gta5GameDataRules::XmlElement>
    {
        try
        {
            return Gta5GameDataRules::ParseXml(ReadText(meta.path));
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("GTA V data: skipping '{}': {}", meta.path.string(), exception.what());
            return std::nullopt;
        }
    };

    for (const Meta& meta : vehicleMetas)
    {
        const std::optional<Gta5GameDataRules::XmlElement> document = parse(meta);
        if (!document)
        {
            continue;
        }
        if (const Gta5GameDataRules::XmlElement* inits = document->Child("InitDatas"))
        {
            for (const Gta5GameDataRules::XmlElement& item : inits->children)
            {
                Gta5VehicleInfo info;
                info.modelName = item.ChildText("modelName");
                if (info.modelName.empty())
                {
                    continue;
                }
                info.txdName = item.ChildText("txdName");
                info.handlingId = item.ChildText("handlingId");
                info.gameName = item.ChildText("gameName");
                info.makeName = item.ChildText("vehicleMakeName");
                info.vehicleClass = item.ChildText("vehicleClass");
                info.type = item.ChildText("type");
                info.wheelScale = item.ChildValue("wheelScale").value_or(0.0f);
                info.wheelScaleRear = item.ChildValue("wheelScaleRear").value_or(info.wheelScale);
                Gta5VehicleInfo& slot = data->m_vehicles[ToLowerAscii(info.modelName)];
                info.colorSets = std::move(slot.colorSets);
                slot = std::move(info);
            }
        }
        if (const Gta5GameDataRules::XmlElement* relationships = document->Child("txdRelationships"))
        {
            for (const Gta5GameDataRules::XmlElement& item : relationships->children)
            {
                const std::string parent = ToLowerAscii(item.ChildText("parent"));
                const std::string child = ToLowerAscii(item.ChildText("child"));
                if (!parent.empty() && !child.empty() && parent != child)
                {
                    data->m_txdParents[child] = parent;
                }
            }
        }
    }

    for (const Meta& meta : variationMetas)
    {
        const std::optional<Gta5GameDataRules::XmlElement> document = parse(meta);
        const Gta5GameDataRules::XmlElement* variations = document ? document->Child("variationData") : nullptr;
        if (variations == nullptr)
        {
            continue;
        }
        for (const Gta5GameDataRules::XmlElement& item : variations->children)
        {
            const Gta5GameDataRules::XmlElement* colors = item.Child("colors");
            const std::string model = ToLowerAscii(item.ChildText("modelName"));
            if (colors == nullptr || model.empty())
            {
                continue;
            }
            std::vector<Gta5ColorSet> sets;
            for (const Gta5GameDataRules::XmlElement& color : colors->children)
            {
                std::istringstream numbers(color.ChildText("indices"));
                Gta5ColorSet set;
                int value = 0;
                for (size_t index = 0; index < set.indices.size() && numbers >> value; ++index)
                {
                    set.indices[index] = value;
                }
                sets.push_back(set);
            }
            if (!sets.empty())
            {
                data->m_vehicles[model].colorSets = std::move(sets);
            }
        }
    }

    for (const Meta& meta : handlingMetas)
    {
        const std::optional<Gta5GameDataRules::XmlElement> document = parse(meta);
        const Gta5GameDataRules::XmlElement* items = document ? document->Child("HandlingData") : nullptr;
        if (items == nullptr)
        {
            continue;
        }
        for (const Gta5GameDataRules::XmlElement& item : items->children)
        {
            Gta5Handling handling = ReadHandling(item);
            if (!handling.name.empty())
            {
                data->m_handlings[ToLowerAscii(handling.name)] = std::move(handling);
            }
        }
    }

    LOG_INFO("GTA V data under '{}': {} vehicles, {} handlings, {} texture dictionary names", root.string(), data->m_vehicles.size(),
             data->m_handlings.size(), data->m_dictionaries.size());
    g_cache[root] = data;
    return data;
}

std::optional<Gta5VehicleInfo> Gta5GameData::Vehicle(const std::string& modelName) const
{
    const auto found = m_vehicles.find(ToLowerAscii(modelName));
    if (found == m_vehicles.end() || found->second.modelName.empty())
    {
        return std::nullopt;
    }
    Gta5VehicleInfo info = found->second;
    if (const auto handling = m_handlings.find(ToLowerAscii(info.handlingId)); handling != m_handlings.end())
    {
        info.handling = handling->second;
    }
    return info;
}

std::vector<std::filesystem::path> Gta5GameData::TextureDictionaries(const std::string& txdName, const std::filesystem::path& near) const
{
    std::vector<std::string> names;
    std::string name = ToLowerAscii(txdName);
    for (size_t depth = 0; !name.empty() && depth < 16; ++depth)
    {
        if (std::find(names.begin(), names.end(), name) != names.end())
        {
            break;
        }
        names.push_back(name);
        const auto parent = m_txdParents.find(name);
        name = parent != m_txdParents.end() ? parent->second : std::string{};
    }
    if (std::find(names.begin(), names.end(), "vehshare") == names.end())
    {
        names.push_back("vehshare");
    }

    const std::string nearPack = PackOf(std::filesystem::absolute(near).lexically_normal().lexically_relative(m_root));
    std::vector<std::filesystem::path> out;
    for (const std::string& dictionary : names)
    {
        for (const std::string& stem : {dictionary + "+hi", dictionary})
        {
            const auto found = m_dictionaries.find(stem);
            if (found == m_dictionaries.end())
            {
                continue;
            }
            std::vector<std::filesystem::path> files = found->second;
            std::stable_sort(files.begin(), files.end(),
                             [&](const std::filesystem::path& a, const std::filesystem::path& b)
                             {
                                 const std::filesystem::path relativeA = a.lexically_relative(m_root);
                                 const std::filesystem::path relativeB = b.lexically_relative(m_root);
                                 const bool nearA = PackOf(relativeA) == nearPack;
                                 const bool nearB = PackOf(relativeB) == nearPack;
                                 if (nearA != nearB)
                                 {
                                     return nearA;
                                 }
                                 return Gta5GameDataRules::MetaPriority(relativeA) > Gta5GameDataRules::MetaPriority(relativeB);
                             });
            out.insert(out.end(), files.begin(), files.end());
        }
    }
    return out;
}

}
