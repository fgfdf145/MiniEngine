#pragma once

#include <imgui.h>

#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace me
{

// A one-colour vector icon from an SVG document: the <path> elements inside its viewBox,
// filled by their fill-rule (nonzero unless "evenodd"). Groups, transforms, strokes and
// styles are not read; the Phosphor icons it draws need none of them.
//
// It is drawn as geometry, flattened for the size it is drawn at, so it is as sharp at
// 200 px as at 16 px; the edges are anti-aliased by a 1 px outline over the fill.
class SvgIcon
{
  public:
    // Null when the document has no viewBox (or width and height) or no drawable path.
    static std::optional<SvgIcon> Parse(std::string_view svg);

    // The viewBox's width over its height.
    float AspectRatio() const
    {
        return m_viewBoxSize.x / m_viewBoxSize.y;
    }

    // The fill at `height` pixels tall, with x from 0 to height * AspectRatio(): trapezoids,
    // four corners each (top-left, top-right, bottom-right, bottom-left), and the outlines
    // of every subpath. Built once per size and kept.
    struct Mesh
    {
        std::vector<ImVec2> quads;
        std::vector<std::vector<ImVec2>> outlines;
    };
    const Mesh& MeshAt(float height) const;

    // The icon `height` pixels tall with its viewBox's top-left corner at `topLeft`.
    void Draw(ImDrawList& drawList, ImVec2 topLeft, float height, ImU32 colour) const;

    // One subpath as the path data spelled it, every segment a cubic Bezier (lines and
    // quadratics raised to cubics, arcs split into quarter arcs): start point, then three
    // points per segment, in viewBox units.
    struct Subpath
    {
        std::vector<ImVec2> points;
        bool closed = false;
    };
    struct Path
    {
        std::vector<Subpath> subpaths;
        bool evenOdd = false;
    };

    // Exposed for tests: the path data `d` as subpaths.
    static std::vector<Subpath> ParsePathData(std::string_view data);

    // An icon of paths read elsewhere (an SVG font's glyph); null without a drawable path.
    static std::optional<SvgIcon> FromPaths(ImVec2 viewBoxMin, ImVec2 viewBoxSize, std::vector<Path> paths);

  private:
    ImVec2 m_viewBoxMin{0.0f, 0.0f};
    ImVec2 m_viewBoxSize{1.0f, 1.0f};
    std::vector<Path> m_paths;
    // By height in quarter pixels; emptied when it holds too many sizes (a viewport being resized
    // draws a scaled HUD at a new size every frame).
    mutable std::map<int, Mesh> m_meshes;
};
}
