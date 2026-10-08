// The GT7 driving HUD and the SVG fonts it is drawn with: SvgFont reads a font's glyphs and character
// references and sets text; the HUD's built-in fonts carry every character it writes; the HUD draws in
// a few states. With MINIENGINE_UI_SNAPSHOT_DIR set the HUD is written there as PNGs:
//   gt7_hud_reference.png  the bottom 300 rows of a 3840 x 2160 frame, to be laid over the same rows
//                          of the game's 4K recording
//   gt7_hud_states.png     cruising; braking on ABS at the limiter; reversing with traction control
//                          cutting, each in a 1280 x 720 viewport

#include <engine/asset/svg_font.h>
#include <engine/asset/svg_icon.h>
#include <engine/editor/ui/editor_gt7_hud.h>

#include <imgui.h>

#include "imgui_software_raster.h"
#include "test_fixture_paths.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me;
using namespace me::test;

namespace
{
void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Require(file.good(), "reads " + path.string());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

const std::filesystem::path kHudFolder = std::filesystem::path(MINIENGINE_SOURCE_DIR) / "engine" / "editor" / "ui" / "gt7_hud";

void TestSvgFont()
{
    // A square "A" (100 wide, cap height 100), an "&" by its named entity, a CJK glyph by a hex
    // reference, a blank space; units per em 200.
    const std::string svg =
        "<svg><defs><font horiz-adv-x='120'><font-face units-per-em='200' ascent='160' descent='40' cap-height='100'/>"
        "<glyph unicode='A' horiz-adv-x='110' d='M0 0H100V100H0Z'/>"
        "<glyph unicode='&amp;' d='M0 0H50V50H0Z'/>"
        "<glyph unicode='&#x516C;' horiz-adv-x='200' d='M0 -20H200V140H0Z'/>"
        "<glyph unicode=' ' horiz-adv-x='60'/>"
        "<glyph unicode='ab' d='M0 0H10V10Z'/>"
        "</font></defs></svg>";
    const std::optional<SvgFont> font = SvgFont::Parse(svg);
    Require(font.has_value(), "the test font parses");
    Require(font->HasGlyph('A') && font->HasGlyph('&') && font->HasGlyph(0x516C) && font->HasGlyph(' '), "glyphs by literal and reference");
    Require(!font->HasGlyph('a'), "a ligature is skipped");
    Require(std::abs(font->SizeForCapHeight(10.0f) - 20.0f) < 1e-4f, "capitals 10 px tall at 20 px per em");
    // At 20 px per em (0.1 px per unit): A 11, space 6, & 12 (the font's default advance), 1 px tracking twice.
    const float width = font->Measure("A &", 20.0f, 1.0f);
    Require(std::abs(width - (11.0f + 6.0f + 12.0f + 2.0f)) < 1e-3f, "measure: " + std::to_string(width));
    Require(std::abs(font->Measure("\xE5\x85\xAC", 20.0f) - 20.0f) < 1e-3f, "a UTF-8 character's advance");
    Require(std::abs(font->Measure("?", 20.0f) - 12.0f) < 1e-3f, "a missing glyph takes the default advance");
    Require(DecodeUtf8("a\xE5\x85\xAC\xF0\x9F\x98\x80") == std::u32string{U'a', 0x516C, 0x1F600}, "UTF-8 decodes");
    Require(DecodeUtf8("\xE5\x85") == std::u32string{0xFFFD, 0xFFFD}, "a cut-off sequence reads as replacements");
    Require(!SvgFont::Parse("<svg><font><glyph unicode='A' d='M0 0H1V1Z'/></font></svg>").has_value(), "no font-face: null");

    ImGui::GetIO().DisplaySize = ImVec2(200.0f, 200.0f);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImDrawList& drawList = *ImGui::GetBackgroundDrawList();
    const int before = drawList.VtxBuffer.Size;
    const float drawn = font->Draw(drawList, ImVec2(100.0f, 100.0f), 20.0f, IM_COL32_WHITE, "A A", SvgFont::Align::Right);
    Require(drawList.VtxBuffer.Size > before && std::abs(drawn - 28.0f) < 1e-3f, "text is drawn as geometry");
    ImGui::EndFrame();
}

void TestHudFonts()
{
    const std::optional<SvgFont> meter = SvgFont::Parse(ReadFile(kHudFolder / "fonts" / "gt7_meter.svg"));
    const std::optional<SvgFont> sans = SvgFont::Parse(ReadFile(kHudFolder / "fonts" / "gt7_sans.svg"));
    const std::optional<SvgFont> bold = SvgFont::Parse(ReadFile(kHudFolder / "fonts" / "gt7_sans_bold.svg"));
    Require(meter && sans && bold, "the HUD's fonts parse");
    for (const char32_t c : std::u32string(U"0123456789NR-.:"))
    {
        Require(meter->HasGlyph(c), "the meter font has U+" + std::to_string(static_cast<uint32_t>(c)));
    }
    for (char32_t c = 0x20; c < 0x7F; ++c)
    {
        Require(sans->HasGlyph(c), "the sans font has U+" + std::to_string(static_cast<uint32_t>(c)));
    }
    for (const char32_t c : std::u32string(U"SMHV0123456789"))
    {
        Require(bold->HasGlyph(c), "the bold font has U+" + std::to_string(static_cast<uint32_t>(c)));
    }
    for (const auto& entry : std::filesystem::directory_iterator(kHudFolder / "icons"))
    {
        const std::optional<SvgIcon> icon = SvgIcon::Parse(ReadFile(entry.path()));
        Require(icon.has_value() && !icon->MeshAt(40.0f).quads.empty(), entry.path().filename().string() + " parses and fills");
    }
    // The meter's digits are wide, as the game's: an 8 is wider than it is tall.
    Require(meter->Measure("8", 100.0f) > 120.0f, "the meter's 8 is wide");
}

// A road-grey ground with a darker band at the top, as the recording's background.
void DrawGround(ImDrawList& drawList, ImVec2 min, ImVec2 max)
{
    drawList.AddRectFilledMultiColor(min, max, IM_COL32(150, 150, 152, 255), IM_COL32(150, 150, 152, 255), IM_COL32(118, 118, 120, 255),
                                     IM_COL32(118, 118, 120, 255));
}

Gt7HudInput Cruising()
{
    Gt7HudInput input;
    input.speedKmh = 88.0f;
    input.rpm = 4200.0f;
    input.shiftRpm = 7600.0f;
    input.gear = 2;
    input.throttle = 1.0f;
    input.steering = 0.04f;
    input.absFitted = true;
    input.tcsFitted = true;
    input.counterSteerAssist = false;
    input.turbo = true;
    input.boostBar = 0.75f;
    input.odometerKm = 31.4;
    input.frontTyre = "SS";
    input.rearTyre = "SS";
    return input;
}

std::vector<float> Render(int width, int height, const std::vector<std::pair<ImVec4, Gt7HudInput>>& views)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImDrawList& drawList = *ImGui::GetBackgroundDrawList();
    for (const auto& [rect, input] : views)
    {
        const ImVec2 min(rect.x, rect.y);
        const ImVec2 size(rect.z, rect.w);
        DrawGround(drawList, min, ImVec2(min.x + size.x, min.y + size.y));
        Require(DrawGt7Hud(drawList, min, size, input), "the HUD draws");
    }
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    return Rasterise(*drawData, width, height);
}

void TestShiftLight()
{
    // Dark below 85 % of the revs to change up at, filling up to them, full and teal at them.
    Require(ComputeGt7ShiftLight(6400.0f, 8000.0f).fill == 0.0f, "the shift light is dark low in the revs");
    const Gt7ShiftLight starting = ComputeGt7ShiftLight(7200.0f, 8000.0f);
    const Gt7ShiftLight nearly = ComputeGt7ShiftLight(7900.0f, 8000.0f);
    Require(starting.fill > 0.2f && starting.fill < nearly.fill && nearly.fill < 1.0f, "the shift light fills with the revs");
    Require((starting.colour & 0xFF) > (starting.colour >> 16 & 0xFF), "it starts orange (more red than blue)");
    Require((nearly.colour >> 16 & 0xFF) > (nearly.colour >> 8 & 0xFF), "it turns lilac (more blue than green)");
    const Gt7ShiftLight full = ComputeGt7ShiftLight(8000.0f, 8000.0f);
    Require(full.fill == 1.0f && full.colour == IM_COL32(120, 190, 195, 255), "at the shift point it is full and teal");
}

void TestHud()
{
    const char* snapshots = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR");

    // In a 3840 x 2160 frame, as the game's own 4K recording, whose bottom 300 rows are the snapshot.
    {
        constexpr int kWidth = 3840;
        constexpr int kHeight = 2160;
        const std::vector<float> image = Render(kWidth, kHeight, {{ImVec4(0.0f, 0.0f, kWidth, kHeight), Cruising()}});
        const auto pixel = [&](int x, int y)
        {
            return image[(static_cast<size_t>(y) * kWidth + x) * 3];
        };
        // The speed's digits are white over the panel, between x 1740 and 1885 at row 1997 as in the
        // game's frame.
        float brightest = 0.0f;
        for (int x = 1740; x < 1885; ++x)
        {
            brightest = std::max(brightest, pixel(x, 1997));
        }
        Require(brightest > 0.85f, "the speed's digits are drawn where the game has them");
        // The brackets beside the pedal bars stand where the game's do (within 2 px): at x 1449 and
        // 2390, from row 1920 to 2100.
        const auto near = [&](int x, int y)
        {
            float value = 0.0f;
            for (int dx = -2; dx <= 2; ++dx)
            {
                value = std::max(value, pixel(x + dx, y));
            }
            return value;
        };
        for (const int x : {1449, 2390})
        {
            Require(near(x, 2010) > 0.8f && near(x, 1910) < 0.65f && near(x, 2110) < 0.65f,
                    "a pedal bracket at x " + std::to_string(x) + " spans the game's rows");
        }
        if (snapshots != nullptr)
        {
            constexpr int kCropHeight = 300;
            const std::vector<float> crop(image.end() - static_cast<std::ptrdiff_t>(kCropHeight) * kWidth * 3, image.end());
            std::filesystem::create_directories(snapshots);
            WritePng(crop, kWidth, kCropHeight, std::filesystem::path(snapshots) / "gt7_hud_reference.png");
        }
    }

    // Three states in 1280 x 720 viewports stacked: cruising; braking on ABS, hand brake on, steering
    // left, at the limiter; reversing slowly with the manual gearbox, traction control cutting.
    {
        Gt7HudInput braking = Cruising();
        braking.speedKmh = 213.0f;
        braking.rpm = 7950.0f;
        braking.gear = 5;
        braking.throttle = 0.0f;
        braking.brake = 0.85f;
        braking.absActive = true;
        braking.handBrake = true;
        braking.steering = -0.6f;
        braking.boostBar = -0.4f;
        Gt7HudInput reversing = Cruising();
        reversing.speedKmh = 7.0f;
        reversing.rpm = 6900.0f;
        reversing.gear = -1;
        reversing.manualGearbox = true;
        reversing.throttle = 0.7f;
        reversing.tcsActive = true;
        reversing.counterSteerAssist = true;
        reversing.boostBar = 1.6f;
        reversing.fuelShare = 0.35f;
        reversing.odometerKm = 123456.78;
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720 * 3;
        const std::vector<float> image = Render(
            kWidth, kHeight,
            {{ImVec4(0.0f, 0.0f, 1280.0f, 720.0f), Cruising()},
             {ImVec4(0.0f, 720.0f, 1280.0f, 720.0f), braking},
             {ImVec4(0.0f, 1440.0f, 1280.0f, 720.0f), reversing}});
        if (snapshots != nullptr)
        {
            WritePng(image, kWidth, kHeight, std::filesystem::path(snapshots) / "gt7_hud_states.png");
        }
    }
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    // Anti-aliased lines as geometry, which the software rasteriser reproduces.
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
    int result = 0;
    try
    {
        TestSvgFont();
        TestHudFonts();
        TestShiftLight();
        TestHud();
        std::cout << "gt7 hud tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "gt7 hud tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
