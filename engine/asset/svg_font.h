#pragma once

#include "svg_icon.h"

#include <imgui.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace me
{

// An SVG 1.1 font (<font> with <font-face> and <glyph unicode= horiz-adv-x= d=>): each glyph's outline
// drawn as an SvgIcon, so text stays sharp at any size, unlike ImGui's font atlas, which is baked at
// the UI's one size. Glyph outlines are y up from the baseline, as the format has them, and filled
// by the nonzero rule. Kerning, ligatures and arabic forms are not read.
class SvgFont
{
  public:
    // Null when the document has no <font-face units-per-em> or no glyph.
    static std::optional<SvgFont> Parse(std::string_view svg);

    float UnitsPerEm() const
    {
        return m_unitsPerEm;
    }

    // The size (pixels per em) at which the font's capitals are `pixels` tall.
    float SizeForCapHeight(float pixels) const
    {
        return pixels * m_unitsPerEm / m_capHeight;
    }

    // How wide `utf8` is at `size` pixels per em, with `tracking` pixels added after every glyph but
    // the last.
    float Measure(std::string_view utf8, float size, float tracking = 0.0f) const;

    enum class Align
    {
        Left,
        Centre,
        Right
    };
    // `utf8` with its baseline on `baseline.y`, starting, centred on or ending at `baseline.x`.
    // Returns its width.
    float Draw(
        ImDrawList& drawList,
        ImVec2 baseline,
        float size,
        ImU32 colour,
        std::string_view utf8,
        Align align = Align::Left,
        float tracking = 0.0f) const;

    bool HasGlyph(char32_t codepoint) const
    {
        return m_glyphs.contains(codepoint);
    }

  private:
    struct Glyph
    {
        float advance = 0.0f;
        std::optional<SvgIcon> outline; // none for a blank (space)
    };
    const Glyph* Find(char32_t codepoint) const;

    float m_unitsPerEm = 1000.0f;
    float m_ascent = 800.0f;
    float m_descent = 200.0f;
    float m_capHeight = 700.0f;
    float m_defaultAdvance = 500.0f;
    std::unordered_map<char32_t, Glyph> m_glyphs;
};

// The code points of UTF-8 text; a malformed byte reads as U+FFFD.
std::u32string DecodeUtf8(std::string_view utf8);
}
