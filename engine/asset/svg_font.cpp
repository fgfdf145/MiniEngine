#include "svg_font.h"
#include "svg_document.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <utility>

namespace me
{

namespace
{
constexpr char32_t kReplacement = 0xFFFD;

std::optional<float> NumberAttribute(std::string_view tag, std::string_view name)
{
    float value = 0.0f;
    if (const std::optional<std::string_view> text = svg::Attribute(tag, name); text && svg::ParseNumber(*text, value))
    {
        return value;
    }
    return std::nullopt;
}

// The text of an attribute with XML's character references and five named entities expanded.
std::string ExpandEntities(std::string_view text)
{
    std::string out;
    size_t i = 0;
    while (i < text.size())
    {
        if (text[i] != '&')
        {
            out += text[i++];
            continue;
        }
        const size_t end = text.find(';', i);
        if (end == std::string_view::npos)
        {
            out += text.substr(i);
            break;
        }
        const std::string_view entity = text.substr(i + 1, end - i - 1);
        char32_t codepoint = 0;
        bool known = true;
        if (entity == "amp")
        {
            codepoint = '&';
        }
        else if (entity == "lt")
        {
            codepoint = '<';
        }
        else if (entity == "gt")
        {
            codepoint = '>';
        }
        else if (entity == "quot")
        {
            codepoint = '"';
        }
        else if (entity == "apos")
        {
            codepoint = '\'';
        }
        else if (entity.size() > 1 && entity[0] == '#')
        {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            const std::string_view digits = entity.substr(hex ? 2 : 1);
            uint32_t value = 0;
            const auto [last, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
            known = error == std::errc() && last == digits.data() + digits.size() && value <= 0x10FFFF;
            codepoint = value;
        }
        else
        {
            known = false;
        }
        if (!known)
        {
            out += text.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        // Back to UTF-8.
        if (codepoint < 0x80)
        {
            out += static_cast<char>(codepoint);
        }
        else if (codepoint < 0x800)
        {
            out += static_cast<char>(0xC0 | (codepoint >> 6));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else if (codepoint < 0x10000)
        {
            out += static_cast<char>(0xE0 | (codepoint >> 12));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (codepoint >> 18));
            out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        i = end + 1;
    }
    return out;
}
}

std::u32string DecodeUtf8(std::string_view utf8)
{
    std::u32string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size())
    {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        int length = 0;
        char32_t codepoint = 0;
        if (byte < 0x80)
        {
            length = 1;
            codepoint = byte;
        }
        else if ((byte & 0xE0) == 0xC0)
        {
            length = 2;
            codepoint = byte & 0x1F;
        }
        else if ((byte & 0xF0) == 0xE0)
        {
            length = 3;
            codepoint = byte & 0x0F;
        }
        else if ((byte & 0xF8) == 0xF0)
        {
            length = 4;
            codepoint = byte & 0x07;
        }
        bool valid = length > 0 && i + static_cast<size_t>(length) <= utf8.size();
        for (int k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(utf8[i + static_cast<size_t>(k)]);
            valid = (next & 0xC0) == 0x80;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (!valid)
        {
            out += kReplacement;
            ++i;
            continue;
        }
        out += codepoint;
        i += static_cast<size_t>(length);
    }
    return out;
}

std::optional<SvgFont> SvgFont::Parse(std::string_view svg)
{
    const std::vector<std::string_view> faces = svg::Tags(svg, "font-face");
    if (faces.empty())
    {
        return std::nullopt;
    }
    SvgFont font;
    const std::optional<float> unitsPerEm = NumberAttribute(faces.front(), "units-per-em");
    if (!unitsPerEm || *unitsPerEm <= 0.0f)
    {
        return std::nullopt;
    }
    font.m_unitsPerEm = *unitsPerEm;
    font.m_ascent = NumberAttribute(faces.front(), "ascent").value_or(font.m_unitsPerEm * 0.8f);
    font.m_descent = std::abs(NumberAttribute(faces.front(), "descent").value_or(font.m_unitsPerEm * 0.2f));
    font.m_capHeight = NumberAttribute(faces.front(), "cap-height").value_or(font.m_ascent * 0.9f);
    if (font.m_capHeight <= 0.0f)
    {
        font.m_capHeight = font.m_ascent;
    }
    if (const std::vector<std::string_view> fonts = svg::Tags(svg, "font"); !fonts.empty())
    {
        font.m_defaultAdvance = NumberAttribute(fonts.front(), "horiz-adv-x").value_or(font.m_unitsPerEm * 0.5f);
    }

    const float height = font.m_ascent + font.m_descent;
    for (const std::string_view tag : svg::Tags(svg, "glyph"))
    {
        const std::optional<std::string_view> unicode = svg::Attribute(tag, "unicode");
        if (!unicode)
        {
            continue;
        }
        const std::u32string codepoints = DecodeUtf8(ExpandEntities(*unicode));
        if (codepoints.size() != 1)
        {
            continue; // ligatures are not drawn
        }
        Glyph glyph;
        glyph.advance = NumberAttribute(tag, "horiz-adv-x").value_or(font.m_defaultAdvance);
        if (const std::optional<std::string_view> data = svg::Attribute(tag, "d"))
        {
            SvgIcon::Path path;
            path.subpaths = SvgIcon::ParsePathData(*data);
            // y up from the baseline to the icon's y down.
            for (SvgIcon::Subpath& subpath : path.subpaths)
            {
                for (ImVec2& point : subpath.points)
                {
                    point.y = -point.y;
                }
            }
            std::vector<SvgIcon::Path> paths;
            paths.push_back(std::move(path));
            glyph.outline = SvgIcon::FromPaths(
                ImVec2(0.0f, -font.m_ascent), ImVec2(std::max(glyph.advance, 1.0f), height), std::move(paths));
        }
        font.m_glyphs[codepoints.front()] = std::move(glyph);
    }
    if (font.m_glyphs.empty())
    {
        return std::nullopt;
    }
    return font;
}

const SvgFont::Glyph* SvgFont::Find(char32_t codepoint) const
{
    if (const auto found = m_glyphs.find(codepoint); found != m_glyphs.end())
    {
        return &found->second;
    }
    return nullptr;
}

float SvgFont::Measure(std::string_view utf8, float size, float tracking) const
{
    const float scale = size / m_unitsPerEm;
    float width = 0.0f;
    const std::u32string text = DecodeUtf8(utf8);
    for (size_t i = 0; i < text.size(); ++i)
    {
        const Glyph* glyph = Find(text[i]);
        width += (glyph != nullptr ? glyph->advance : m_defaultAdvance) * scale;
        if (i + 1 < text.size())
        {
            width += tracking;
        }
    }
    return width;
}

float SvgFont::Draw(
    ImDrawList& drawList,
    ImVec2 baseline,
    float size,
    ImU32 colour,
    std::string_view utf8,
    Align align,
    float tracking) const
{
    const float width = Measure(utf8, size, tracking);
    float x = baseline.x;
    if (align == Align::Centre)
    {
        x -= width * 0.5f;
    }
    else if (align == Align::Right)
    {
        x -= width;
    }
    const float scale = size / m_unitsPerEm;
    const float top = baseline.y - m_ascent * scale;
    const float height = (m_ascent + m_descent) * scale;
    for (const char32_t codepoint : DecodeUtf8(utf8))
    {
        const Glyph* glyph = Find(codepoint);
        if (glyph != nullptr && glyph->outline)
        {
            glyph->outline->Draw(drawList, ImVec2(x, top), height, colour);
        }
        x += (glyph != nullptr ? glyph->advance : m_defaultAdvance) * scale + tracking;
    }
    return width;
}
}
