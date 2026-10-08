#include "editor_gt7_hud.h"

#include <engine/asset/svg_font.h>
#include <engine/asset/svg_icon.h>
#include <engine/core/log/log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

// The HUD's SVGs, built into the editor by cmake/MiniEngineEmbedFile.cmake (engine/editor/CMakeLists.txt).
#define ME_GT7_HUD_SVGS(X)                                                                                                  \
    X(gt7_meter)                                                                                                         \
    X(gt7_sans)                                                                                                          \
    X(gt7_sans_bold)                                                                                                     \
    X(abs)                                                                                                               \
    X(arrow_left)                                                                                                        \
    X(arrow_right)                                                                                                       \
    X(brake_assist)                                                                                                          X(car_lines)                                                                                                         \
    X(car_markers)                                                                                                       \
    X(car_top)                                                                                                           \
    X(countersteer)                                                                                                      \
    X(fuel)                                                                                                              \
    X(handbrake)                                                                                                         \
    X(headlight)                                                                                                         \
    X(steering)                                                                                                          \
    X(tcs)                                                                                                               \
    X(throttle)                                                                                                          \
    X(turbo)                                                                                                             \
    X(warning)                                                                                                           \
    X(water)

namespace me
{
#define ME_DECLARE_GT7_HUD_SVG(name)                                                                                     \
    extern const unsigned char kGt7HudSvg_##name[];                                                                     \
    extern const std::size_t kGt7HudSvg_##name##Size;
ME_GT7_HUD_SVGS(ME_DECLARE_GT7_HUD_SVG)
#undef ME_DECLARE_GT7_HUD_SVG

namespace
{
// The layout is in the pixels of a still of the game's HUD (a 2000 x 356 strip), x 1135 the screen's
// centre. Measured against a whole 3840 x 2160 frame of the game (the brackets beside the pedal bars
// and the surface water's), the still was 1.283 times smaller than the screen: the screen is 2992 x
// 1683 of its pixels, the bottom edge at y 359.
constexpr float kReferenceCentreX = 1135.0f;
constexpr float kReferenceBottomY = 359.0f;
constexpr float kReferenceWidth = 2992.0f;
constexpr float kReferenceHeight = 1683.0f;
// Below this scale the strip would be too small to read.
constexpr float kMinScale = 0.2f;

// The labels as the game's English HUD shows them: the speed's unit, and the gearbox, automatic or
// manual.
constexpr const char* kKilometresPerHour = "km/h";
constexpr const char* kAutomatic = "AT";
constexpr const char* kManual = "MT";

// Colours, opaque wherever shapes overlap (glyphs, icons), so nothing shows through twice.
constexpr ImU32 kWhite = IM_COL32(242, 242, 242, 255);
constexpr ImU32 kLabel = IM_COL32(228, 228, 228, 255);
constexpr ImU32 kTextOutline = IM_COL32(20, 20, 20, 200);
constexpr ImU32 kBracket = IM_COL32(236, 236, 236, 235);
constexpr ImU32 kPanel = IM_COL32(34, 34, 34, 150);
constexpr ImU32 kTickOff = IM_COL32(52, 52, 52, 220);
constexpr ImU32 kRed = IM_COL32(232, 40, 46, 255);
constexpr ImU32 kTrack = IM_COL32(58, 58, 58, 190);
constexpr ImU32 kDisc = IM_COL32(170, 170, 170, 120);
constexpr ImU32 kGearBox = IM_COL32(46, 46, 46, 215);
constexpr ImU32 kDivider = IM_COL32(178, 178, 178, 210);
constexpr ImU32 kBlindSide = IM_COL32(74, 74, 74, 150);
constexpr ImU32 kArrow = IM_COL32(226, 226, 226, 235);
constexpr ImU32 kTyre = IM_COL32(236, 236, 236, 255);
constexpr ImU32 kTyreFrame = IM_COL32(46, 46, 46, 205);
constexpr ImU32 kCar = IM_COL32(62, 62, 62, 225);
constexpr ImU32 kCarLines = IM_COL32(150, 150, 150, 255);
constexpr ImU32 kCarMarkers = IM_COL32(58, 58, 58, 235);

// The lamps: an assist that is off is grey; one that is on is white, outlined (ABS) or filled (the
// TCS, counter-steer); one working this moment is red.
enum class Lamp
{
    Off,
    On,
    OnFilled,
    Active,
    ActiveFilled
};

struct Assets
{
    std::optional<SvgFont> meter;
    std::optional<SvgFont> sans;
    std::optional<SvgFont> sansBold;
    std::optional<SvgIcon> abs, arrowLeft, arrowRight, brakeAssist, carLines, carMarkers, carTop, countersteer, fuel, handbrake,
        headlight, steering, tcs, throttle, turbo, warning, water;

    bool Complete() const
    {
        return meter && sans && sansBold && abs && arrowLeft && arrowRight && brakeAssist && carLines && carMarkers && carTop && countersteer &&
               fuel && handbrake && headlight && steering && tcs && throttle && turbo && warning && water;
    }
};

std::string_view Embedded(const unsigned char* bytes, std::size_t size)
{
    return std::string_view(reinterpret_cast<const char*>(bytes), size);
}

// Parsed on first use; the editor draws on one thread.
const Assets& GetAssets()
{
    static const Assets assets = []
    {
        Assets loaded;
#define ME_GT7_SVG(name) Embedded(kGt7HudSvg_##name, kGt7HudSvg_##name##Size)
        loaded.meter = SvgFont::Parse(ME_GT7_SVG(gt7_meter));
        loaded.sans = SvgFont::Parse(ME_GT7_SVG(gt7_sans));
        loaded.sansBold = SvgFont::Parse(ME_GT7_SVG(gt7_sans_bold));
        loaded.abs = SvgIcon::Parse(ME_GT7_SVG(abs));
        loaded.arrowLeft = SvgIcon::Parse(ME_GT7_SVG(arrow_left));
        loaded.arrowRight = SvgIcon::Parse(ME_GT7_SVG(arrow_right));
        loaded.brakeAssist = SvgIcon::Parse(ME_GT7_SVG(brake_assist));
        loaded.carLines = SvgIcon::Parse(ME_GT7_SVG(car_lines));
        loaded.carMarkers = SvgIcon::Parse(ME_GT7_SVG(car_markers));
        loaded.carTop = SvgIcon::Parse(ME_GT7_SVG(car_top));
        loaded.countersteer = SvgIcon::Parse(ME_GT7_SVG(countersteer));
        loaded.fuel = SvgIcon::Parse(ME_GT7_SVG(fuel));
        loaded.handbrake = SvgIcon::Parse(ME_GT7_SVG(handbrake));
        loaded.headlight = SvgIcon::Parse(ME_GT7_SVG(headlight));
        loaded.steering = SvgIcon::Parse(ME_GT7_SVG(steering));
        loaded.tcs = SvgIcon::Parse(ME_GT7_SVG(tcs));
        loaded.throttle = SvgIcon::Parse(ME_GT7_SVG(throttle));
        loaded.turbo = SvgIcon::Parse(ME_GT7_SVG(turbo));
        loaded.warning = SvgIcon::Parse(ME_GT7_SVG(warning));
        loaded.water = SvgIcon::Parse(ME_GT7_SVG(water));
#undef ME_GT7_SVG
        if (!loaded.Complete())
        {
            LOG_ERROR("The GT7 HUD's built-in SVGs did not all parse; the HUD is off");
        }
        return loaded;
    }();
    return assets;
}

ImVec2 operator+(ImVec2 a, ImVec2 b)
{
    return ImVec2(a.x + b.x, a.y + b.y);
}

float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

class Painter
{
  public:
    Painter(ImDrawList& drawList, ImVec2 anchor, float scale)
        : m_drawList(drawList)
        , m_anchor(anchor)
        , m_scale(scale)
    {
    }

    // A point of the recording on the screen.
    ImVec2 P(float x, float y) const
    {
        return ImVec2(m_anchor.x + (x - kReferenceCentreX) * m_scale, m_anchor.y + (y - kReferenceBottomY) * m_scale);
    }

    float L(float length) const
    {
        return length * m_scale;
    }

    void Line(float x0, float y0, float x1, float y1, ImU32 colour, float thickness = 2.2f)
    {
        m_drawList.AddLine(P(x0, y0), P(x1, y1), colour, std::max(L(thickness), 1.0f));
    }

    void Rect(float x0, float y0, float x1, float y1, ImU32 colour, float rounding = 0.0f)
    {
        m_drawList.AddRectFilled(P(x0, y0), P(x1, y1), colour, L(rounding));
    }

    void Disc(float x, float y, float radius, ImU32 colour)
    {
        m_drawList.AddCircleFilled(P(x, y), L(radius), colour, 0);
    }

    // An arc between two angles (radians, clockwise from +x on the screen).
    void Arc(float x, float y, float radius, float from, float to, ImU32 colour, float thickness)
    {
        if (std::abs(to - from) < 1e-4f)
        {
            return;
        }
        m_drawList.PathArcTo(P(x, y), L(radius), std::min(from, to), std::max(from, to), 0);
        m_drawList.PathStroke(colour, std::max(L(thickness), 1.0f));
    }

    // An icon `height` tall centred on (x, y).
    void Icon(const SvgIcon& icon, float x, float y, float height, ImU32 colour)
    {
        const float h = L(height);
        const ImVec2 centre = P(x, y);
        icon.Draw(m_drawList, ImVec2(centre.x - h * icon.AspectRatio() * 0.5f, centre.y - h * 0.5f), h, colour);
    }

    // Text with its capitals `capHeight` tall on the baseline at y.
    void Text(const SvgFont& font, float x, float baseline, float capHeight, ImU32 colour, std::string_view text,
              SvgFont::Align align = SvgFont::Align::Left, float tracking = 0.0f)
    {
        font.Draw(m_drawList, P(x, baseline), font.SizeForCapHeight(L(capHeight)), colour, text, align, L(tracking));
    }

    // The same with a dark rim, for text over the road.
    void OutlinedText(const SvgFont& font, float x, float baseline, float capHeight, ImU32 colour, std::string_view text,
                      SvgFont::Align align)
    {
        const float rim = std::max(L(1.6f), 1.0f);
        const float size = font.SizeForCapHeight(L(capHeight));
        const ImVec2 at = P(x, baseline);
        for (const ImVec2 offset : {ImVec2(-rim, 0.0f), ImVec2(rim, 0.0f), ImVec2(0.0f, -rim), ImVec2(0.0f, rim)})
        {
            font.Draw(m_drawList, at + offset, size, kTextOutline, text, align);
        }
        font.Draw(m_drawList, at, size, colour, text, align);
    }

    void LampAt(const SvgIcon& icon, float x, float y, Lamp lamp)
    {
        constexpr float kRadius = 25.0f;
        constexpr float kIcon = 40.0f;
        switch (lamp)
        {
        case Lamp::Off:
            Disc(x, y, kRadius, IM_COL32(150, 150, 150, 105));
            Icon(icon, x, y, kIcon, IM_COL32(54, 54, 54, 235));
            break;
        case Lamp::On:
            Disc(x, y, kRadius, IM_COL32(44, 44, 44, 170));
            Icon(icon, x, y, kIcon, kWhite);
            break;
        case Lamp::OnFilled:
            Disc(x, y, kRadius, kWhite);
            Icon(icon, x, y, kIcon, IM_COL32(40, 40, 40, 255));
            break;
        case Lamp::Active:
            Disc(x, y, kRadius, IM_COL32(44, 44, 44, 170));
            Icon(icon, x, y, kIcon, kRed);
            break;
        case Lamp::ActiveFilled:
            Disc(x, y, kRadius, kRed);
            Icon(icon, x, y, kIcon, kWhite);
            break;
        }
    }

    ImDrawList& DrawList()
    {
        return m_drawList;
    }

  private:
    ImDrawList& m_drawList;
    ImVec2 m_anchor;
    float m_scale;
};

// The bracket beside a pedal's bar: a line with long ticks at its ends and a short one halfway, the
// ticks pointing away from the bar (`side` -1 to the left, +1 to the right).
void DrawPedalBar(Painter& p, float lineX, float side, float value, float cutShare, bool cut)
{
    constexpr float kTop = 173.0f;
    constexpr float kBottom = 313.0f;
    constexpr float kMiddle = 243.0f;
    p.Line(lineX, kTop, lineX, kBottom, kBracket);
    p.Line(lineX, kTop, lineX + side * 40.0f, kTop, kBracket);
    p.Line(lineX, kBottom, lineX + side * 40.0f, kBottom, kBracket);
    p.Line(lineX, kMiddle, lineX + side * 16.0f, kMiddle, kBracket);

    // The bar, beside the line on the ticks' side, filling from the bottom.
    constexpr float kBarTop = 178.0f;
    constexpr float kBarBottom = 308.0f;
    const float inner = lineX + side * 7.0f;
    const float outer = lineX + side * 24.0f;
    const float x0 = std::min(inner, outer);
    const float x1 = std::max(inner, outer);
    const float top = kBarBottom - (kBarBottom - kBarTop) * Clamp01(value);
    if (value > 0.0f)
    {
        p.Rect(x0, top, x1, kBarBottom, kWhite);
        // What the assist takes off, red at the top of the bar.
        if (cut)
        {
            p.Rect(x0, top, x1, top + (kBarBottom - top) * cutShare, kRed);
        }
    }
}

void DrawTyres(Painter& p, const Assets& assets, const Gt7HudInput& input)
{
    // Surface water: a bracket with the bar it would fill (dry here) and its icon.
    p.Line(125.0f, 173.0f, 125.0f, 313.0f, kBracket);
    p.Line(85.0f, 173.0f, 125.0f, 173.0f, kBracket);
    p.Line(105.0f, 220.0f, 125.0f, 220.0f, kBracket);
    p.Line(105.0f, 267.0f, 125.0f, 267.0f, kBracket);
    p.Line(85.0f, 313.0f, 125.0f, 313.0f, kBracket);
    p.Icon(*assets.water, 72.0f, 243.0f, 30.0f, kLabel);

    // The four tyres round the car, white while healthy, in a dark frame.
    struct TyreBox
    {
        float x0, y0, x1, y1;
    };
    constexpr TyreBox kTyres[] = {
        {175.0f, 168.0f, 203.0f, 232.0f},
        {305.0f, 168.0f, 333.0f, 232.0f},
        {175.0f, 255.0f, 203.0f, 318.0f},
        {305.0f, 255.0f, 333.0f, 318.0f},
    };
    for (const TyreBox& tyre : kTyres)
    {
        p.Rect(tyre.x0 - 5.0f, tyre.y0 - 5.0f, tyre.x1 + 5.0f, tyre.y1 + 5.0f, kTyreFrame, 6.0f);
        p.Rect(tyre.x0, tyre.y0, tyre.x1, tyre.y1, kTyre, 2.5f);
    }
    p.Icon(*assets.carTop, 254.0f, 243.0f, 115.0f, kCar);
    p.Icon(*assets.carLines, 254.0f, 243.0f, 115.0f, kCarLines);
    p.Icon(*assets.carMarkers, 254.0f, 243.0f, 115.0f, kCarMarkers);
    if (!input.frontTyre.empty())
    {
        p.OutlinedText(*assets.sansBold, 254.0f, 173.0f, 16.0f, kWhite, input.frontTyre, SvgFont::Align::Centre);
    }
    if (!input.rearTyre.empty())
    {
        p.OutlinedText(*assets.sansBold, 254.0f, 328.0f, 16.0f, kWhite, input.rearTyre, SvgFont::Align::Centre);
    }
}

void DrawFuelAndOdometer(Painter& p, const Assets& assets, const Gt7HudInput& input)
{
    constexpr float kX = 455.0f;
    constexpr float kY = 243.0f;
    constexpr float kRadius = 58.0f;
    constexpr float kPi = std::numbers::pi_v<float>;
    // A half circle over the pump, empty at the left (E), full at the right (F).
    p.Arc(kX, kY, kRadius, kPi, 2.0f * kPi, kTrack, 8.0f);
    p.Arc(kX, kY, kRadius, kPi, kPi * (1.0f + Clamp01(input.fuelShare)), kWhite, 8.0f);
    for (int quarter = 0; quarter <= 4; ++quarter)
    {
        const float angle = kPi * (1.0f + 0.25f * static_cast<float>(quarter));
        const float length = quarter % 2 == 0 ? 11.0f : 6.0f;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        p.Line(kX + c * (kRadius - 4.0f), kY + s * (kRadius - 4.0f), kX + c * (kRadius - 4.0f - length), kY + s * (kRadius - 4.0f - length), kWhite, 2.4f);
    }
    p.Text(*assets.sans, 380.0f, 250.0f, 14.0f, kLabel, "E", SvgFont::Align::Centre);
    p.Text(*assets.sans, 531.0f, 250.0f, 14.0f, kLabel, "F", SvgFont::Align::Centre);
    p.Disc(kX, kY, 22.0f, kDisc);
    p.Icon(*assets.fuel, kX, kY, 28.0f, kLabel);

    // The odometer: kilometres to a tenth, six digits before the point.
    p.Rect(381.0f, 284.0f, 530.0f, 315.0f, IM_COL32(42, 42, 42, 195), 3.0f);
    char odometer[32];
    std::snprintf(odometer, sizeof(odometer), "%08.1f", std::clamp(input.odometerKm, 0.0, 999999.9));
    p.Text(*assets.sans, 455.5f, 308.0f, 17.0f, kWhite, odometer, SvgFont::Align::Centre, 1.5f);
}

void DrawRevPanel(Painter& p, const Assets& assets, const Gt7HudInput& input)
{
    // The panel: flat on top under the rev strip, its lower corners cut off.
    const ImVec2 panel[] = {
        p.P(826.0f, 197.0f), p.P(1443.0f, 197.0f), p.P(1443.0f, 228.0f),
        p.P(1370.0f, 303.0f), p.P(900.0f, 303.0f), p.P(826.0f, 228.0f),
    };
    p.DrawList().AddConvexPolyFilled(panel, 6, kPanel);

    // The rev strip: a comb of ticks whose tops arc up to the middle; the shift light (Gt7ShiftLight)
    // fills it from the left.
    constexpr int kTicks = 101;
    constexpr float kLeft = 829.0f;
    constexpr float kRight = 1440.0f;
    constexpr float kBase = 197.0f;
    constexpr float kCrest = 160.0f;
    constexpr float kArcRadius = 2661.0f;
    const Gt7ShiftLight light = ComputeGt7ShiftLight(input.rpm, input.shiftRpm);
    const int litTicks = static_cast<int>(std::ceil(light.fill * static_cast<float>(kTicks) - 1e-3f));
    for (int tick = 0; tick < kTicks; ++tick)
    {
        const float x = kLeft + (kRight - kLeft) * static_cast<float>(tick) / static_cast<float>(kTicks - 1);
        const float dx = x - kReferenceCentreX;
        const float top = kCrest + kArcRadius - std::sqrt(kArcRadius * kArcRadius - dx * dx);
        p.Line(x, top, x, kBase, tick < litTicks ? light.colour : kTickOff, 2.2f);
    }
    // The steering: a red dot beside the grey centre mark.
    p.Disc(kReferenceCentreX, 141.0f, 4.2f, IM_COL32(205, 205, 205, 255));
    p.Disc(kReferenceCentreX + 180.0f * std::clamp(input.steering, -1.0f, 1.0f), 141.0f, 4.6f, IM_COL32(255, 46, 30, 255));

    // The blind-side indicators, unlit: rings in the panel's lower corners, cut off by its edges.
    const auto inPanel = [&](ImVec2 point)
    {
        for (int i = 0; i < 6; ++i)
        {
            const ImVec2 a = panel[i];
            const ImVec2 b = panel[(i + 1) % 6];
            if ((b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x) < 0.0f)
            {
                return false;
            }
        }
        return true;
    };
    for (const float centreX : {868.0f, 1400.0f})
    {
        for (const float radius : {11.0f, 19.0f, 27.0f})
        {
            constexpr int kSteps = 96;
            std::vector<ImVec2> run;
            for (int step = 0; step <= kSteps; ++step)
            {
                const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(step) / static_cast<float>(kSteps);
                const ImVec2 point = p.P(centreX + radius * std::cos(angle), 262.0f + radius * std::sin(angle));
                if (inPanel(point))
                {
                    run.push_back(point);
                }
                if ((!inPanel(point) || step == kSteps) && run.size() >= 2)
                {
                    p.DrawList().AddPolyline(run.data(), static_cast<int>(run.size()), kBlindSide, std::max(p.L(3.5f), 1.0f));
                }
                if (!inPanel(point) || step == kSteps)
                {
                    run.clear();
                }
            }
        }
    }

    // Speed and gear either side of the divider.
    char speed[16];
    std::snprintf(speed, sizeof(speed), "%d", static_cast<int>(std::abs(input.speedKmh) + 0.5f));
    p.Text(*assets.meter, 1108.0f, 252.0f, 40.0f, kWhite, speed, SvgFont::Align::Right);
    p.Text(*assets.sans, 1110.0f, 288.0f, 14.2f, kLabel, kKilometresPerHour, SvgFont::Align::Right);
    p.Line(kReferenceCentreX, 195.0f, kReferenceCentreX, 292.0f, kDivider, 2.0f);
    char gear[8];
    if (input.gear < 0)
    {
        std::snprintf(gear, sizeof(gear), "R");
    }
    else if (input.gear == 0)
    {
        std::snprintf(gear, sizeof(gear), "N");
    }
    else
    {
        std::snprintf(gear, sizeof(gear), "%d", std::min(input.gear, 99));
    }
    p.Text(*assets.meter, 1162.0f, 275.0f, 65.0f, kWhite, gear);
    // The suggested gear's box, empty: there is no braking suggestion.
    p.Rect(1270.0f, 208.0f, 1337.0f, 258.0f, kGearBox, 4.0f);
    p.Text(*assets.sans, 1303.5f, 288.0f, 14.2f, kLabel, input.manualGearbox ? kManual : kAutomatic, SvgFont::Align::Centre);

    p.Icon(*assets.arrowLeft, 829.0f, 302.0f, 30.0f, kArrow);
    p.Icon(*assets.arrowRight, 1440.0f, 302.0f, 30.0f, kArrow);
}

void DrawBoost(Painter& p, const Assets& assets, const Gt7HudInput& input)
{
    constexpr float kX = 1812.0f;
    constexpr float kY = 245.0f;
    constexpr float kRadius = 57.0f;
    constexpr float kPi = std::numbers::pi_v<float>;
    // -1 at the bottom, 0 at the left, 1 at the top, 2 at the right.
    const auto angle = [](float bar)
    {
        return kPi * 0.5f + (bar + 1.0f) * kPi * 0.5f;
    };
    p.Arc(kX, kY, kRadius, angle(-1.0f), angle(2.0f), kTrack, 6.0f);
    const float boost = std::clamp(input.boostBar, -1.0f, 2.0f);
    p.Arc(kX, kY, kRadius, angle(0.0f), angle(boost), kWhite, 6.0f);
    const auto tick = [&](float bar, float length, float thickness, ImU32 colour)
    {
        const float a = angle(bar);
        const float c = std::cos(a);
        const float s = std::sin(a);
        const float inner = kRadius - 3.0f - length;
        p.Line(kX + c * (kRadius - 3.0f), kY + s * (kRadius - 3.0f), kX + c * inner, kY + s * inner, colour, thickness);
    };
    // The vacuum's tenths, then the whole bars.
    for (int tenth = 1; tenth < 10; ++tenth)
    {
        tick(-1.0f + 0.1f * static_cast<float>(tenth), 6.0f, 1.6f, IM_COL32(222, 222, 222, 230));
    }
    tick(-1.0f, 10.0f, 2.4f, kWhite);
    tick(0.0f, 16.0f, 3.2f, kWhite);
    tick(1.0f, 10.0f, 2.4f, kWhite);
    tick(2.0f, 12.0f, 2.4f, kWhite);
    p.Text(*assets.sans, kX, 172.0f, 14.0f, kLabel, "1", SvgFont::Align::Centre);
    p.Text(*assets.sans, 1737.0f, 250.0f, 14.0f, kLabel, "0", SvgFont::Align::Centre);
    p.Text(*assets.sans, 1888.0f, 250.0f, 14.0f, kLabel, "2", SvgFont::Align::Centre);
    p.Text(*assets.sans, kX, 327.0f, 14.0f, kLabel, "-1", SvgFont::Align::Centre);
    p.Disc(kX, 243.0f, 22.0f, kDisc);
    p.Icon(*assets.turbo, kX, 243.0f, 32.0f, kLabel);
    p.Text(*assets.sans, 1836.0f, 283.0f, 13.0f, kLabel, "x100");
    p.Text(*assets.sans, 1836.0f, 304.0f, 13.0f, kLabel, "kPa");
}
}

bool DrawGt7Hud(ImDrawList& drawList, ImVec2 origin, ImVec2 size, const Gt7HudInput& input)
{
    const Assets& assets = GetAssets();
    if (!assets.Complete())
    {
        return false;
    }
    const float scale = std::min(size.y / kReferenceHeight, size.x / kReferenceWidth);
    if (scale < kMinScale)
    {
        return true;
    }
    drawList.PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    Painter p(drawList, ImVec2(origin.x + size.x * 0.5f, origin.y + size.y), scale);

    DrawTyres(p, assets, input);
    DrawFuelAndOdometer(p, assets, input);

    // ABS (on while fitted, red while it lets a brake off), the hand brake (red while pulled), and the
    // auto-drive brakes and steering, which this car has not.
    p.LampAt(*assets.abs, 597.0f, 212.0f, !input.absFitted ? Lamp::Off : input.absActive ? Lamp::Active : Lamp::On);
    p.LampAt(*assets.handbrake, 658.0f, 212.0f, input.handBrake ? Lamp::Active : Lamp::Off);
    p.LampAt(*assets.brakeAssist, 597.0f, 274.0f, Lamp::Off);
    p.LampAt(*assets.steering, 658.0f, 274.0f, Lamp::Off);

    p.Icon(*assets.brakeAssist, 718.0f, 243.0f, 26.0f, kLabel);
    DrawPedalBar(p, 768.0f, -1.0f, input.brake, 0.3f, input.absActive);

    DrawRevPanel(p, assets, input);

    DrawPedalBar(p, 1503.0f, 1.0f, input.throttle, 1.0f, input.tcsActive);
    p.Icon(*assets.throttle, 1555.0f, 243.0f, 26.0f, kLabel);

    // Lights (none), counter-steer assist, traction control and stability management (none).
    p.LampAt(*assets.headlight, 1612.0f, 212.0f, Lamp::Off);
    p.LampAt(*assets.countersteer, 1673.0f, 212.0f, input.counterSteerAssist ? Lamp::OnFilled : Lamp::Off);
    p.LampAt(*assets.tcs, 1612.0f, 274.0f, !input.tcsFitted ? Lamp::Off : input.tcsActive ? Lamp::ActiveFilled : Lamp::OnFilled);
    p.LampAt(*assets.warning, 1673.0f, 274.0f, Lamp::Off);

    if (input.turbo)
    {
        DrawBoost(p, assets, input);
    }
    drawList.PopClipRect();
    return true;
}

Gt7ShiftLight ComputeGt7ShiftLight(float rpm, float shiftRpm)
{
    // The colours along the fill and when full, from the game's 4K recording (its HUD washed out by the
    // capture, saturated here to match the steering dot's red).
    struct Stop
    {
        float fill;
        float r, g, b;
    };
    constexpr Stop kStops[] = {
        {0.0f, 255.0f, 70.0f, 25.0f},
        {0.5f, 250.0f, 60.0f, 40.0f},
        {0.65f, 220.0f, 75.0f, 100.0f},
        {0.8f, 180.0f, 100.0f, 155.0f},
        {1.0f, 170.0f, 125.0f, 185.0f},
    };
    constexpr ImU32 kFull = IM_COL32(120, 190, 195, 255);
    constexpr float kStart = 0.85f;

    Gt7ShiftLight light;
    const float shift = std::max(shiftRpm, 1000.0f);
    if (rpm >= shift)
    {
        light.fill = 1.0f;
        light.colour = kFull;
        return light;
    }
    light.fill = Clamp01((rpm - kStart * shift) / ((1.0f - kStart) * shift));
    const Stop* upper = std::find_if(std::begin(kStops), std::end(kStops), [&](const Stop& stop)
                                     {
                                         return stop.fill >= light.fill;
                                     });
    const Stop* lower = upper == std::begin(kStops) ? upper : upper - 1;
    const float t = upper->fill > lower->fill ? (light.fill - lower->fill) / (upper->fill - lower->fill) : 0.0f;
    const auto mix = [&](float a, float b)
    {
        return static_cast<int>(std::lround(a + (b - a) * t));
    };
    light.colour = IM_COL32(mix(lower->r, upper->r), mix(lower->g, upper->g), mix(lower->b, upper->b), 255);
    return light;
}
}
