// SvgIcon: SVG path data read command by command, filled by its fill rule (areas checked
// against the exact ones), and the Assets window's icons drawn at several sizes. With
// MINIENGINE_UI_SNAPSHOT_DIR set the icons are written there as a PNG.

#include <engine/asset/svg_icon.h>

#include <imgui.h>

#include "imgui_software_raster.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
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

// The filled area of `path` in a 10 x 10 viewBox, in path units: drawn 1000 px tall, so the
// curves' flattening (0.2 px) costs next to nothing.
double FilledArea(const std::string& path, const std::string& extraAttributes = "")
{
    const std::string svg = "<svg viewBox=\"0 0 10 10\"><path " + extraAttributes + " d=\"" + path + "\"/></svg>";
    const std::optional<SvgIcon> icon = SvgIcon::Parse(svg);
    Require(icon.has_value(), "parses " + svg);
    const SvgIcon::Mesh& mesh = icon->MeshAt(1000.0f);
    double area = 0.0;
    for (size_t i = 0; i + 3 < mesh.quads.size(); i += 4)
    {
        const ImVec2* q = &mesh.quads[i];
        area += 0.5 * ((q[1].x - q[0].x) + (q[2].x - q[3].x)) * (q[3].y - q[0].y);
    }
    return area / (100.0 * 100.0);
}

void RequireArea(const std::string& what, double area, double expected, double tolerance)
{
    std::cout << what << ": area " << area << " (expected " << expected << ")\n";
    Require(std::abs(area - expected) <= tolerance, what + ": area " + std::to_string(area) + ", expected " + std::to_string(expected));
}

void TestFillRules()
{
    const std::string outer = "M0 0H10V10H0Z";
    RequireArea("square", FilledArea(outer), 100.0, 1e-3);
    // A hole drawn the same way round fills under nonzero and stays open under even-odd.
    const std::string sameWay = outer + "M3 3H7V7H3Z";
    RequireArea("nonzero, hole the same way round", FilledArea(sameWay), 100.0, 1e-3);
    RequireArea("evenodd, hole the same way round", FilledArea(sameWay, "fill-rule=\"evenodd\""), 84.0, 1e-3);
    RequireArea("evenodd from style", FilledArea(sameWay, "style=\"fill-rule:evenodd\""), 84.0, 1e-3);
    // Drawn the other way round it is a hole under both.
    RequireArea("nonzero, hole the other way round", FilledArea(outer + "M3 3V7H7V3Z"), 84.0, 1e-3);
    // Two overlapping squares: their union under nonzero, crossing edges and all.
    RequireArea("overlap", FilledArea("M0 0H6V6H0Z M4 4H10V10H4Z"), 68.0, 1e-3);
    // A bow tie: its edges cross mid-band.
    RequireArea("bow tie", FilledArea("M0 0L10 10H0L10 0Z"), 50.0, 1e-3);
}

void TestPathCommands()
{
    RequireArea("relative moves and lines", FilledArea("m1 1 h8 v8 h-8 z"), 64.0, 1e-3);
    RequireArea("implicit line-tos after a move, commas", FilledArea("M0,0 10,0 10,10 0,10z"), 100.0, 1e-3);
    RequireArea("numbers run together", FilledArea("M0-0L10-0L10 10L0 10Z"), 100.0, 1e-3);
    RequireArea("an unclosed path is filled closed", FilledArea("M0 0H10V10H0"), 100.0, 1e-3);
    // The segment under a parabola is 2/3 of the rectangle round it.
    RequireArea("quadratic", FilledArea("M0 10Q5 0 10 10Z"), 2.0 / 3.0 * 10.0 * 5.0, 0.05);
    RequireArea("smooth quadratic", FilledArea("M0 10Q2.5 0 5 10T10 10Z"), 2.0 * (2.0 / 3.0 * 5.0 * 5.0), 0.05);
    // A cubic hump with its control points straight above its ends encloses 3/5 of base times
    // control height; S mirrors the first hump's last control point into the lower hump.
    RequireArea("cubic", FilledArea("M0 10C0 0 5 0 5 10Z"), 3.0 / 5.0 * 5.0 * 10.0, 0.05);
    RequireArea("cubic and smooth cubic", FilledArea("M0 5C0 0 10 0 10 5S0 10 0 5Z"), 2.0 * (3.0 / 5.0 * 10.0 * 5.0), 0.05);
    const double circle = std::numbers::pi * 25.0;
    RequireArea("circle of two arcs", FilledArea("M0 5A5 5 0 1 0 10 5A5 5 0 1 0 0 5Z"), circle, circle * 0.005);
    RequireArea("arc flags run together", FilledArea("M0 5a5 5 0 1010 0a5 5 0 10-10 0z"), circle, circle * 0.005);
    // Radii too small for the end points grow: a half circle of radius 5.
    RequireArea("radii grown to reach", FilledArea("M0 5A1 1 0 0 0 10 5Z"), circle * 0.5, circle * 0.005);
    RequireArea("a zero radius arc is a line", FilledArea("M0 0H10A0 0 0 0 1 10 10H0Z"), 100.0, 1e-3);
}

void TestMalformedDocuments()
{
    Require(!SvgIcon::Parse("<svg><path d=\"M0 0H10V10Z\"/></svg>").has_value(), "no viewBox nor size: null");
    Require(!SvgIcon::Parse("<svg viewBox=\"0 0 10 10\"></svg>").has_value(), "no path: null");
    Require(!SvgIcon::Parse("not svg").has_value(), "not SVG: null");
    Require(SvgIcon::Parse("<svg width=\"20\" height=\"10\"><path d=\"M0 0H20V10Z\"/></svg>")->AspectRatio() == 2.0f,
            "width and height stand in for the viewBox");
    // Bad data keeps what was read before it; "id" is not "d".
    const std::optional<SvgIcon> partial = SvgIcon::Parse("<svg viewBox=\"0 0 10 10\"><path id=\"x\" d=\"M0 0H10V10H0Z M3 3 X 4\"/></svg>");
    Require(partial.has_value() && !partial->MeshAt(10.0f).quads.empty(), "data before an unknown command is kept");
}

// The Assets window's icons at 16, 32, 64 and 160 pixels; each fills part of its box (at least a
// twentieth), and a larger size flattens its curves into more pieces.
void TestAssetIcons()
{
    const std::filesystem::path folder = std::filesystem::path(MINIENGINE_SOURCE_DIR) / "engine" / "asset" / "icons";
    const std::vector<std::string> names = {"folder", "model", "material", "scene", "texture", "file", "parent_folder", "audio"};
    const std::vector<float> sizes = {16.0f, 32.0f, 64.0f, 160.0f};
    constexpr int kWidth = 1840;
    constexpr int kHeight = 180;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImDrawList& drawList = *ImGui::GetBackgroundDrawList();
    float x = 10.0f;
    for (const std::string& name : names)
    {
        std::ifstream file(folder / (name + ".svg"));
        std::stringstream text;
        text << file.rdbuf();
        const std::optional<SvgIcon> icon = SvgIcon::Parse(text.str());
        Require(icon.has_value(), name + ".svg parses");
        size_t previousPoints = 0;
        float y = 10.0f;
        for (const float size : sizes)
        {
            const SvgIcon::Mesh& mesh = icon->MeshAt(size);
            double area = 0.0;
            for (size_t i = 0; i + 3 < mesh.quads.size(); i += 4)
            {
                const ImVec2* q = &mesh.quads[i];
                area += 0.5 * ((q[1].x - q[0].x) + (q[2].x - q[3].x)) * (q[3].y - q[0].y);
            }
            const double box = static_cast<double>(size) * size * icon->AspectRatio();
            size_t points = 0;
            for (const std::vector<ImVec2>& outline : mesh.outlines)
            {
                points += outline.size();
            }
            // Phosphor's regular icons are drawn lines, so a thin one (the parent folder's arrow) inks
            // well under a tenth of its box; an empty or broken fill still fails.
            Require(area > 0.05 * box && area < box, name + " fills part of its box at " + std::to_string(size) + " px");
            Require(points >= previousPoints, name + " has at least as many outline points at a larger size");
            previousPoints = points;
            if (size == sizes.back())
            {
                std::cout << name << ": " << mesh.quads.size() / 4 << " trapezoids, " << points << " outline points at " << size
                          << " px, fills " << static_cast<int>(100.0 * area / box) << "% of its box\n";
            }
            // 16, 32 and 64 px stacked in one column, 160 px beside them.
            icon->Draw(drawList, ImVec2(x, y), size, IM_COL32(115, 191, 255, 255));
            y += size + 10.0f;
            if (size == 64.0f)
            {
                y = 10.0f;
                x += 64.0f * icon->AspectRatio() + 10.0f;
            }
        }
        x += 160.0f * icon->AspectRatio() + 20.0f;
    }
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    if (const char* snapshots = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
    {
        std::filesystem::create_directories(snapshots);
        WritePng(Rasterise(*drawData, kWidth, kHeight), kWidth, kHeight, std::filesystem::path(snapshots) / "svg_icons.png");
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
    // Anti-aliased lines as geometry with a fading fringe, which the software rasteriser
    // reproduces; the textured kind needs the GPU's filtering.
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
    int result = 0;
    try
    {
        TestFillRules();
        TestPathCommands();
        TestMalformedDocuments();
        TestAssetIcons();
        std::cout << "svg icon tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "svg icon tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
