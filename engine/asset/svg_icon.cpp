#include "svg_icon.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <string>

namespace me
{

namespace
{
// Curves are flattened so no chord strays further than this from the curve, in pixels.
constexpr float kFlatteningTolerance = 0.2f;

ImVec2 operator+(ImVec2 a, ImVec2 b)
{
    return ImVec2(a.x + b.x, a.y + b.y);
}

ImVec2 operator-(ImVec2 a, ImVec2 b)
{
    return ImVec2(a.x - b.x, a.y - b.y);
}

ImVec2 operator*(ImVec2 a, float s)
{
    return ImVec2(a.x * s, a.y * s);
}

float Length(ImVec2 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y);
}

// Reads the numbers and flags of SVG path data and attribute lists, where "1.5.5" is two
// numbers, "-1-2" too, and commas count as white space.
class Scanner
{
  public:
    explicit Scanner(std::string_view text)
        : m_text(text)
    {
    }

    void SkipSeparators()
    {
        while (m_position < m_text.size() &&
               (std::isspace(static_cast<unsigned char>(m_text[m_position])) != 0 || m_text[m_position] == ','))
        {
            ++m_position;
        }
    }

    bool AtEnd()
    {
        SkipSeparators();
        return m_position >= m_text.size();
    }

    char Peek()
    {
        SkipSeparators();
        return m_position < m_text.size() ? m_text[m_position] : '\0';
    }

    char Take()
    {
        return m_text[m_position++];
    }

    bool AtNumber()
    {
        const char c = Peek();
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+' || c == '.';
    }

    bool Number(float& value)
    {
        SkipSeparators();
        const size_t begin = m_position;
        size_t end = begin;
        const auto digits = [&]()
        {
            while (end < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[end])) != 0)
            {
                ++end;
            }
        };
        if (end < m_text.size() && (m_text[end] == '-' || m_text[end] == '+'))
        {
            ++end;
        }
        digits();
        if (end < m_text.size() && m_text[end] == '.')
        {
            ++end;
            digits();
        }
        if (end < m_text.size() && (m_text[end] == 'e' || m_text[end] == 'E'))
        {
            size_t exponent = end + 1;
            if (exponent < m_text.size() && (m_text[exponent] == '-' || m_text[exponent] == '+'))
            {
                ++exponent;
            }
            if (exponent < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[exponent])) != 0)
            {
                end = exponent;
                digits();
            }
        }
        // from_chars takes no leading '+'.
        const size_t parseBegin = (begin < end && m_text[begin] == '+') ? begin + 1 : begin;
        const auto [last, error] = std::from_chars(m_text.data() + parseBegin, m_text.data() + end, value);
        if (error != std::errc() || last != m_text.data() + end)
        {
            return false;
        }
        m_position = end;
        return true;
    }

    // An arc flag: one '0' or '1', which may run straight into what follows ("a1 1 0 01.5 2").
    bool Flag(bool& value)
    {
        const char c = Peek();
        if (c != '0' && c != '1')
        {
            return false;
        }
        value = c == '1';
        ++m_position;
        return true;
    }

  private:
    std::string_view m_text;
    size_t m_position = 0;
};

// Builds the subpaths, every segment as a cubic.
class PathBuilder
{
  public:
    void MoveTo(ImVec2 point)
    {
        m_subpaths.push_back({{point}, false});
        m_start = point;
        m_point = point;
        m_open = true;
    }

    void LineTo(ImVec2 point)
    {
        const ImVec2 from = Begin();
        CubicTo(from + (point - from) * (1.0f / 3.0f), from + (point - from) * (2.0f / 3.0f), point);
    }

    void QuadTo(ImVec2 control, ImVec2 point)
    {
        const ImVec2 from = Begin();
        CubicTo(from + (control - from) * (2.0f / 3.0f), point + (control - point) * (2.0f / 3.0f), point);
    }

    void CubicTo(ImVec2 control1, ImVec2 control2, ImVec2 point)
    {
        Begin();
        std::vector<ImVec2>& points = m_subpaths.back().points;
        points.push_back(control1);
        points.push_back(control2);
        points.push_back(point);
        m_point = point;
    }

    // The elliptical arc of SVG's "A" command, by the endpoint-to-centre conversion of the
    // SVG specification (appendix F.6.5), in quarter arcs approximated by cubics.
    void ArcTo(float rx, float ry, float rotationDegrees, bool largeArc, bool sweep, ImVec2 point)
    {
        const ImVec2 from = Begin();
        if (from.x == point.x && from.y == point.y)
        {
            return;
        }
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx == 0.0f || ry == 0.0f)
        {
            LineTo(point);
            return;
        }
        const float phi = rotationDegrees * std::numbers::pi_v<float> / 180.0f;
        const float cosPhi = std::cos(phi);
        const float sinPhi = std::sin(phi);
        const float halfDx = (from.x - point.x) * 0.5f;
        const float halfDy = (from.y - point.y) * 0.5f;
        const float x1 = cosPhi * halfDx + sinPhi * halfDy;
        const float y1 = -sinPhi * halfDx + cosPhi * halfDy;
        // Radii too small to reach the end point grow until they just do.
        const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1.0f)
        {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const float numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const float denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        float coefficient = denominator > 0.0f ? std::sqrt(std::max(0.0f, numerator / denominator)) : 0.0f;
        if (largeArc == sweep)
        {
            coefficient = -coefficient;
        }
        const float cx1 = coefficient * rx * y1 / ry;
        const float cy1 = -coefficient * ry * x1 / rx;
        const ImVec2 centre(
            cosPhi * cx1 - sinPhi * cy1 + (from.x + point.x) * 0.5f,
            sinPhi * cx1 + cosPhi * cy1 + (from.y + point.y) * 0.5f);

        const auto angle = [](ImVec2 u, ImVec2 v)
        {
            return std::atan2(u.x * v.y - u.y * v.x, u.x * v.x + u.y * v.y);
        };
        const float startAngle = angle(ImVec2(1.0f, 0.0f), ImVec2((x1 - cx1) / rx, (y1 - cy1) / ry));
        float sweepAngle = angle(ImVec2((x1 - cx1) / rx, (y1 - cy1) / ry), ImVec2((-x1 - cx1) / rx, (-y1 - cy1) / ry));
        const float fullTurn = 2.0f * std::numbers::pi_v<float>;
        if (!sweep && sweepAngle > 0.0f)
        {
            sweepAngle -= fullTurn;
        }
        else if (sweep && sweepAngle < 0.0f)
        {
            sweepAngle += fullTurn;
        }

        const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweepAngle) / (std::numbers::pi_v<float> * 0.5f) - 1e-4f)));
        const float step = sweepAngle / static_cast<float>(segments);
        const float handle = 4.0f / 3.0f * std::tan(step * 0.25f);
        const auto onEllipse = [&](ImVec2 unit)
        {
            return ImVec2(
                centre.x + cosPhi * rx * unit.x - sinPhi * ry * unit.y,
                centre.y + sinPhi * rx * unit.x + cosPhi * ry * unit.y);
        };
        for (int segment = 0; segment < segments; ++segment)
        {
            const float a0 = startAngle + step * static_cast<float>(segment);
            const float a1 = a0 + step;
            const ImVec2 e0(std::cos(a0), std::sin(a0));
            const ImVec2 e1(std::cos(a1), std::sin(a1));
            const ImVec2 control1 = e0 + ImVec2(-e0.y, e0.x) * handle;
            const ImVec2 control2 = e1 - ImVec2(-e1.y, e1.x) * handle;
            CubicTo(onEllipse(control1), onEllipse(control2), segment + 1 == segments ? point : onEllipse(e1));
        }
    }

    void Close()
    {
        if (m_subpaths.empty() || !m_open)
        {
            return;
        }
        if (m_point.x != m_start.x || m_point.y != m_start.y)
        {
            LineTo(m_start);
        }
        m_subpaths.back().closed = true;
        m_point = m_start;
        // Drawing on after "Z" without a move starts a new subpath at the same point.
        m_open = false;
    }

    ImVec2 Point() const
    {
        return m_point;
    }

    std::vector<SvgIcon::Subpath> Take()
    {
        return std::move(m_subpaths);
    }

  private:
    // The current point, starting a subpath there when none is open.
    ImVec2 Begin()
    {
        if (m_subpaths.empty() || !m_open)
        {
            m_subpaths.push_back({{m_point}, false});
            m_start = m_point;
        }
        m_open = true;
        return m_point;
    }

    std::vector<SvgIcon::Subpath> m_subpaths;
    ImVec2 m_start{0.0f, 0.0f};
    ImVec2 m_point{0.0f, 0.0f};
    bool m_open = false;
};

// The value of attribute `name` in the tag text `tag` ("<path d='...' ...>"), if present.
std::optional<std::string_view> Attribute(std::string_view tag, std::string_view name)
{
    size_t search = 0;
    while (true)
    {
        const size_t found = tag.find(name, search);
        if (found == std::string_view::npos)
        {
            return std::nullopt;
        }
        search = found + 1;
        // A whole attribute name: white space before it ("d" is not the end of "id").
        if (found == 0 || std::isspace(static_cast<unsigned char>(tag[found - 1])) == 0)
        {
            continue;
        }
        size_t cursor = found + name.size();
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0)
        {
            ++cursor;
        }
        if (cursor >= tag.size() || tag[cursor] != '=')
        {
            continue;
        }
        ++cursor;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0)
        {
            ++cursor;
        }
        if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\''))
        {
            continue;
        }
        const char quote = tag[cursor];
        const size_t end = tag.find(quote, cursor + 1);
        if (end == std::string_view::npos)
        {
            return std::nullopt;
        }
        return tag.substr(cursor + 1, end - cursor - 1);
    }
}

// The text of each element `<name ...>` in the document, up to its closing '>'.
std::vector<std::string_view> Tags(std::string_view document, std::string_view name)
{
    std::vector<std::string_view> tags;
    const std::string open = "<" + std::string(name);
    size_t search = 0;
    while (true)
    {
        const size_t begin = document.find(open, search);
        if (begin == std::string_view::npos)
        {
            break;
        }
        const size_t after = begin + open.size();
        const size_t end = document.find('>', after);
        if (end == std::string_view::npos)
        {
            break;
        }
        if (after < document.size() && (std::isspace(static_cast<unsigned char>(document[after])) != 0 ||
                                        document[after] == '>' || document[after] == '/'))
        {
            tags.push_back(document.substr(begin, end - begin));
        }
        search = end;
    }
    return tags;
}

// Drops the points where a closed outline goes straight on or turns straight back ("H448H64H448"
// in some icon sets' paths): they add nothing to the fill, and the anti-aliasing outline would
// fold over itself there and leave a notch.
void RemoveStraightPoints(std::vector<ImVec2>& contour)
{
    bool removed = true;
    while (removed && contour.size() >= 3)
    {
        removed = false;
        for (size_t i = 0; i < contour.size() && contour.size() >= 3; ++i)
        {
            const ImVec2 previous = contour[(i + contour.size() - 1) % contour.size()];
            const ImVec2 next = contour[(i + 1) % contour.size()];
            const ImVec2 in = contour[i] - previous;
            const ImVec2 out = next - contour[i];
            const float cross = in.x * out.y - in.y * out.x;
            if (std::abs(cross) <= 1e-4f * Length(in) * Length(out))
            {
                contour.erase(contour.begin() + static_cast<std::ptrdiff_t>(i));
                removed = true;
                --i;
            }
        }
    }
}

// A polygon's edges as the sweep needs them: from the upper end (smaller y) down.
struct Edge
{
    float top;
    float bottom;
    float xAtTop;
    float slope; // dx/dy
    int winding; // +1 where the contour runs down, -1 up

    float XAt(float y) const
    {
        return xAtTop + (y - top) * slope;
    }
};

// Fills the contours by `evenOdd` (else nonzero) as trapezoids: the plane is cut into bands
// at every vertex and every crossing of two edges, so within a band no edges cross and the
// filled spans between them are exact trapezoids. Holes and overlaps come out right.
void Tessellate(const std::vector<std::vector<ImVec2>>& contours, bool evenOdd, std::vector<ImVec2>& quads)
{
    std::vector<Edge> edges;
    std::vector<float> cuts;
    for (const std::vector<ImVec2>& contour : contours)
    {
        for (size_t i = 0; i < contour.size(); ++i)
        {
            const ImVec2 a = contour[i];
            const ImVec2 b = contour[(i + 1) % contour.size()];
            cuts.push_back(a.y);
            if (a.y == b.y)
            {
                continue;
            }
            const bool down = a.y < b.y;
            const ImVec2 upper = down ? a : b;
            const ImVec2 lower = down ? b : a;
            edges.push_back({upper.y, lower.y, upper.x, (lower.x - upper.x) / (lower.y - upper.y), down ? 1 : -1});
        }
    }
    for (size_t i = 0; i < edges.size(); ++i)
    {
        for (size_t j = i + 1; j < edges.size(); ++j)
        {
            const float top = std::max(edges[i].top, edges[j].top);
            const float bottom = std::min(edges[i].bottom, edges[j].bottom);
            if (bottom <= top)
            {
                continue;
            }
            const float d0 = edges[i].XAt(top) - edges[j].XAt(top);
            const float d1 = edges[i].XAt(bottom) - edges[j].XAt(bottom);
            if ((d0 < 0.0f && d1 > 0.0f) || (d0 > 0.0f && d1 < 0.0f))
            {
                cuts.push_back(top + (bottom - top) * d0 / (d0 - d1));
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](float a, float b)
                           {
                               return b - a < 1e-4f;
                           }),
               cuts.end());

    struct Crossing
    {
        float xMid;
        float xTop;
        float xBottom;
        int winding;
    };
    std::vector<Crossing> crossings;
    for (size_t band = 0; band + 1 < cuts.size(); ++band)
    {
        const float top = cuts[band];
        const float bottom = cuts[band + 1];
        const float middle = (top + bottom) * 0.5f;
        crossings.clear();
        for (const Edge& edge : edges)
        {
            if (edge.top < middle && edge.bottom > middle)
            {
                crossings.push_back({edge.XAt(middle), edge.XAt(top), edge.XAt(bottom), edge.winding});
            }
        }
        std::sort(crossings.begin(), crossings.end(), [](const Crossing& a, const Crossing& b)
                  {
                      return a.xMid < b.xMid;
                  });
        int winding = 0;
        const Crossing* left = nullptr;
        for (const Crossing& crossing : crossings)
        {
            const bool wasInside = evenOdd ? (winding & 1) != 0 : winding != 0;
            winding += crossing.winding;
            const bool isInside = evenOdd ? (winding & 1) != 0 : winding != 0;
            if (!wasInside && isInside)
            {
                left = &crossing;
            }
            else if (wasInside && !isInside && left != nullptr)
            {
                quads.push_back(ImVec2(left->xTop, top));
                quads.push_back(ImVec2(crossing.xTop, top));
                quads.push_back(ImVec2(crossing.xBottom, bottom));
                quads.push_back(ImVec2(left->xBottom, bottom));
            }
        }
    }
}
}

std::vector<SvgIcon::Subpath> SvgIcon::ParsePathData(std::string_view data)
{
    PathBuilder builder;
    Scanner scanner(data);
    char command = '\0';
    ImVec2 lastCubicControl{0.0f, 0.0f};
    ImVec2 lastQuadControl{0.0f, 0.0f};
    char previous = '\0';
    while (!scanner.AtEnd())
    {
        if (std::isalpha(static_cast<unsigned char>(scanner.Peek())) != 0)
        {
            command = scanner.Take();
        }
        else if (command == '\0' || !scanner.AtNumber())
        {
            break; // malformed: keep what was read
        }
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const ImVec2 origin = relative ? builder.Point() : ImVec2(0.0f, 0.0f);
        const auto point = [&](ImVec2& out)
        {
            float x = 0.0f;
            float y = 0.0f;
            if (!scanner.Number(x) || !scanner.Number(y))
            {
                return false;
            }
            out = ImVec2(x, y) + origin;
            return true;
        };
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        bool ok = true;
        switch (upper)
        {
        case 'M':
        {
            ImVec2 to;
            ok = point(to);
            if (ok)
            {
                builder.MoveTo(to);
                // Further pairs after a move are lines.
                command = relative ? 'l' : 'L';
            }
            break;
        }
        case 'L':
        {
            ImVec2 to;
            ok = point(to);
            if (ok)
            {
                builder.LineTo(to);
            }
            break;
        }
        case 'H':
        case 'V':
        {
            float value = 0.0f;
            ok = scanner.Number(value);
            if (ok)
            {
                ImVec2 to = builder.Point();
                if (upper == 'H')
                {
                    to.x = value + (relative ? to.x : 0.0f);
                }
                else
                {
                    to.y = value + (relative ? to.y : 0.0f);
                }
                builder.LineTo(to);
            }
            break;
        }
        case 'C':
        case 'S':
        {
            ImVec2 control1;
            ImVec2 control2;
            ImVec2 to;
            if (upper == 'C')
            {
                ok = point(control1) && point(control2) && point(to);
            }
            else
            {
                const ImVec2 from = builder.Point();
                const bool reflects = previous == 'C' || previous == 'S';
                control1 = reflects ? from + (from - lastCubicControl) : from;
                ok = point(control2) && point(to);
            }
            if (ok)
            {
                builder.CubicTo(control1, control2, to);
                lastCubicControl = control2;
            }
            break;
        }
        case 'Q':
        case 'T':
        {
            ImVec2 control;
            ImVec2 to;
            if (upper == 'Q')
            {
                ok = point(control) && point(to);
            }
            else
            {
                const ImVec2 from = builder.Point();
                const bool reflects = previous == 'Q' || previous == 'T';
                control = reflects ? from + (from - lastQuadControl) : from;
                ok = point(to);
            }
            if (ok)
            {
                builder.QuadTo(control, to);
                lastQuadControl = control;
            }
            break;
        }
        case 'A':
        {
            float rx = 0.0f;
            float ry = 0.0f;
            float rotation = 0.0f;
            bool largeArc = false;
            bool sweep = false;
            ImVec2 to;
            ok = scanner.Number(rx) && scanner.Number(ry) && scanner.Number(rotation) && scanner.Flag(largeArc) &&
                 scanner.Flag(sweep) && point(to);
            if (ok)
            {
                builder.ArcTo(rx, ry, rotation, largeArc, sweep, to);
            }
            break;
        }
        case 'Z':
            builder.Close();
            break;
        default:
            ok = false;
            break;
        }
        if (!ok)
        {
            break;
        }
        previous = upper;
    }
    return builder.Take();
}

std::optional<SvgIcon> SvgIcon::Parse(std::string_view svg)
{
    const std::vector<std::string_view> svgTags = Tags(svg, "svg");
    if (svgTags.empty())
    {
        return std::nullopt;
    }
    SvgIcon icon;
    if (const std::optional<std::string_view> viewBox = Attribute(svgTags.front(), "viewBox"))
    {
        Scanner scanner(*viewBox);
        float values[4] = {};
        for (float& value : values)
        {
            if (!scanner.Number(value))
            {
                return std::nullopt;
            }
        }
        icon.m_viewBoxMin = ImVec2(values[0], values[1]);
        icon.m_viewBoxSize = ImVec2(values[2], values[3]);
    }
    else
    {
        const std::optional<std::string_view> width = Attribute(svgTags.front(), "width");
        const std::optional<std::string_view> height = Attribute(svgTags.front(), "height");
        float w = 0.0f;
        float h = 0.0f;
        if (!width || !height || !Scanner(*width).Number(w) || !Scanner(*height).Number(h))
        {
            return std::nullopt;
        }
        icon.m_viewBoxSize = ImVec2(w, h);
    }
    if (icon.m_viewBoxSize.x <= 0.0f || icon.m_viewBoxSize.y <= 0.0f)
    {
        return std::nullopt;
    }

    for (const std::string_view tag : Tags(svg, "path"))
    {
        const std::optional<std::string_view> data = Attribute(tag, "d");
        if (!data)
        {
            continue;
        }
        Path path;
        path.subpaths = ParsePathData(*data);
        const std::optional<std::string_view> fillRule = Attribute(tag, "fill-rule");
        const std::optional<std::string_view> style = Attribute(tag, "style");
        path.evenOdd = (fillRule && *fillRule == "evenodd") ||
                       (style && style->find("fill-rule:evenodd") != std::string_view::npos);
        if (!path.subpaths.empty())
        {
            icon.m_paths.push_back(std::move(path));
        }
    }
    if (icon.m_paths.empty())
    {
        return std::nullopt;
    }
    return icon;
}

const SvgIcon::Mesh& SvgIcon::MeshAt(float height) const
{
    const int key = std::max(1, static_cast<int>(std::lround(height * 4.0f)));
    if (const auto found = m_meshes.find(key); found != m_meshes.end())
    {
        return found->second;
    }

    const float scale = (static_cast<float>(key) * 0.25f) / m_viewBoxSize.y;
    Mesh mesh;
    for (const Path& path : m_paths)
    {
        std::vector<std::vector<ImVec2>> contours;
        for (const Subpath& subpath : path.subpaths)
        {
            const auto toPixels = [&](ImVec2 p)
            {
                return (p - m_viewBoxMin) * scale;
            };
            std::vector<ImVec2> contour{toPixels(subpath.points.front())};
            for (size_t i = 1; i + 2 < subpath.points.size(); i += 3)
            {
                const ImVec2 p0 = contour.back();
                const ImVec2 c1 = toPixels(subpath.points[i]);
                const ImVec2 c2 = toPixels(subpath.points[i + 1]);
                const ImVec2 p3 = toPixels(subpath.points[i + 2]);
                // A cubic strays from its chords by at most 3/4 of this over the square of their count.
                const float bend = std::max(Length(p0 - c1 * 2.0f + c2), Length(c1 - c2 * 2.0f + p3));
                const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75f * bend / kFlatteningTolerance))), 1, 64);
                for (int piece = 1; piece <= pieces; ++piece)
                {
                    const float t = static_cast<float>(piece) / static_cast<float>(pieces);
                    const float u = 1.0f - t;
                    const ImVec2 p = p0 * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) + p3 * (t * t * t);
                    if (std::abs(p.x - contour.back().x) > 1e-4f || std::abs(p.y - contour.back().y) > 1e-4f)
                    {
                        contour.push_back(p);
                    }
                }
            }
            // A fill closes every subpath.
            while (contour.size() > 1 && std::abs(contour.back().x - contour.front().x) <= 1e-4f &&
                   std::abs(contour.back().y - contour.front().y) <= 1e-4f)
            {
                contour.pop_back();
            }
            RemoveStraightPoints(contour);
            if (contour.size() >= 3)
            {
                contours.push_back(std::move(contour));
            }
        }
        Tessellate(contours, path.evenOdd, mesh.quads);
        for (std::vector<ImVec2>& contour : contours)
        {
            mesh.outlines.push_back(std::move(contour));
        }
    }
    return m_meshes.emplace(key, std::move(mesh)).first->second;
}

void SvgIcon::Draw(ImDrawList& drawList, ImVec2 topLeft, float height, ImU32 colour) const
{
    if (height <= 0.0f || (colour & IM_COL32_A_MASK) == 0)
    {
        return;
    }
    const Mesh& mesh = MeshAt(height);
    // Whole pixels, so the outline's anti-aliasing lands the same way at every position.
    const ImVec2 origin(std::round(topLeft.x), std::round(topLeft.y));
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    const int quadCount = static_cast<int>(mesh.quads.size() / 4);
    if (quadCount > 0)
    {
        drawList.PrimReserve(quadCount * 6, quadCount * 4);
        for (int quad = 0; quad < quadCount; ++quad)
        {
            const ImVec2* corners = &mesh.quads[static_cast<size_t>(quad) * 4];
            drawList.PrimQuadUV(
                corners[0] + origin,
                corners[1] + origin,
                corners[2] + origin,
                corners[3] + origin,
                white,
                white,
                white,
                white,
                colour);
        }
    }
    // The fill's edges are hard; a hairline along them, over the fill, softens them.
    std::vector<ImVec2> outline;
    for (const std::vector<ImVec2>& contour : mesh.outlines)
    {
        outline.resize(contour.size());
        for (size_t i = 0; i < contour.size(); ++i)
        {
            outline[i] = contour[i] + origin;
        }
        drawList.AddPolyline(outline.data(), static_cast<int>(outline.size()), colour, 1.0f, ImDrawFlags_Closed);
    }
}
}
