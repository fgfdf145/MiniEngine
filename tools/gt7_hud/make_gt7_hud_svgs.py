"""Writes the GT7-style driving HUD's SVG files into engine/editor/ui/gt7_hud.

    python tools/gt7_hud/make_gt7_hud_svgs.py

Everything the HUD draws that is not a plain bar, arc or tick is one of these SVGs, drawn by
SvgIcon / SvgFont as filled geometry, so it stays sharp at any size:

  fonts/gt7_meter.svg      the meter's thin, wide digits (speed, gear), drawn here as strokes and
                           filled; an SVG 1.1 font
  fonts/gt7_sans.svg       Noto Sans SC at weight 350 (Latin and the few Chinese labels), an SVG font
  fonts/gt7_sans_bold.svg  the same at weight 700, Latin only (tyre compounds)
  icons/*.svg              the warning lamps and gauge icons, one colour each

Needs fontTools and shapely, and Noto Sans SC (C:/Windows/Fonts/NotoSansSC-VF.ttf on Windows, or pass
--noto). The font is under the SIL Open Font License 1.1; fonts/OFL.txt carries it.
"""

import argparse
import math
from pathlib import Path

from fontTools.pens.basePen import BasePen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from shapely import affinity
from shapely.geometry import LineString, LinearRing, MultiPolygon, Point, Polygon
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "engine" / "editor" / "ui" / "gt7_hud"

# Chinese labels the HUD uses (km/h, automatic / manual gearbox).
CJK_LABELS = "公里/小时自动挡手动挡"


# ---------------------------------------------------------------------------------------------
# Geometry helpers


def fmt(value):
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def path_data(geometry, flip_y=False):
    """SVG path data of a (multi)polygon: every ring a closed subpath."""
    polygons = []
    if geometry.is_empty:
        return ""
    if isinstance(geometry, Polygon):
        polygons = [geometry]
    elif isinstance(geometry, MultiPolygon):
        polygons = list(geometry.geoms)
    else:
        polygons = [g for g in getattr(geometry, "geoms", []) if isinstance(g, Polygon)]
    parts = []
    for polygon in polygons:
        polygon = orient(polygon, 1.0)
        for ring in [polygon.exterior, *polygon.interiors]:
            coords = list(ring.coords)[:-1]
            if len(coords) < 3:
                continue
            points = [(x, -y if flip_y else y) for x, y in coords]
            parts.append("M" + " L".join(f"{fmt(x)} {fmt(y)}" for x, y in points) + "Z")
    return "".join(parts)


def arc_points(cx, cy, r, start_deg, end_deg, step_deg=3.0):
    """Points on a circle, angles in degrees clockwise from +x in y-down space."""
    count = max(2, int(math.ceil(abs(end_deg - start_deg) / step_deg)) + 1)
    return [
        (cx + r * math.cos(math.radians(start_deg + (end_deg - start_deg) * i / (count - 1))),
         cy + r * math.sin(math.radians(start_deg + (end_deg - start_deg) * i / (count - 1))))
        for i in range(count)
    ]


def fillet(points, radius, sharp=(), closed=False):
    """A polyline with its corners rounded by `radius` (indices in `sharp` stay sharp)."""
    n = len(points)
    out = []
    for i in range(n):
        p = points[i]
        interior = closed or 0 < i < n - 1
        if not interior or i in sharp or radius <= 0:
            out.append(p)
            continue
        prev = points[(i - 1) % n]
        nxt = points[(i + 1) % n]
        ux, uy = prev[0] - p[0], prev[1] - p[1]
        wx, wy = nxt[0] - p[0], nxt[1] - p[1]
        lu = math.hypot(ux, uy)
        lw = math.hypot(wx, wy)
        ux, uy, wx, wy = ux / lu, uy / lu, wx / lw, wy / lw
        cos_angle = max(-1.0, min(1.0, ux * wx + uy * wy))
        half = math.acos(cos_angle) * 0.5
        if half < 1e-3 or abs(half - math.pi / 2) < 1e-3:
            out.append(p)
            continue
        tangent = min(radius / math.tan(half), lu * 0.5, lw * 0.5)
        r = tangent * math.tan(half)
        a = (p[0] + ux * tangent, p[1] + uy * tangent)
        b = (p[0] + wx * tangent, p[1] + wy * tangent)
        bx, by = ux + wx, uy + wy
        lb = math.hypot(bx, by)
        centre_distance = r / math.sin(half)
        c = (p[0] + bx / lb * centre_distance, p[1] + by / lb * centre_distance)
        start = math.atan2(a[1] - c[1], a[0] - c[0])
        end = math.atan2(b[1] - c[1], b[0] - c[0])
        delta = end - start
        while delta > math.pi:
            delta -= 2 * math.pi
        while delta < -math.pi:
            delta += 2 * math.pi
        steps = max(4, int(abs(delta) / math.radians(4)))
        for s in range(steps + 1):
            t = start + delta * s / steps
            out.append((c[0] + r * math.cos(t), c[1] + r * math.sin(t)))
    return out


def stroke(points, width, closed=False, cap="flat", join="mitre"):
    if closed:
        return Polygon(points).exterior.buffer(width / 2, join_style=join, mitre_limit=4.0)
    return LineString(points).buffer(width / 2, cap_style=cap, join_style=join, mitre_limit=4.0)


def ring(cx, cy, r, width):
    return Point(cx, cy).buffer(r + width / 2, quad_segs=32).difference(Point(cx, cy).buffer(r - width / 2, quad_segs=32))


def disc(cx, cy, r):
    return Point(cx, cy).buffer(r, quad_segs=32)


def arc(cx, cy, r, start_deg, end_deg, width, cap="flat"):
    return stroke(arc_points(cx, cy, r, start_deg, end_deg), width, cap=cap)


# ---------------------------------------------------------------------------------------------
# Font outlines (fontTools) as shapely geometry


class FlatteningPen(BasePen):
    """Collects a glyph's contours as polylines, curves cut into short chords."""

    def __init__(self, glyph_set, steps=12):
        super().__init__(glyph_set)
        self.contours = []
        self.current = []
        self.steps = steps

    def _moveTo(self, pt):
        self.current = [pt]

    def _lineTo(self, pt):
        self.current.append(pt)

    def _curveToOne(self, p1, p2, p3):
        p0 = self.current[-1]
        for i in range(1, self.steps + 1):
            t = i / self.steps
            u = 1 - t
            self.current.append((
                u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))

    def _qCurveToOne(self, p1, p2):
        p0 = self.current[-1]
        for i in range(1, self.steps + 1):
            t = i / self.steps
            u = 1 - t
            self.current.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                                 u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))

    def _closePath(self):
        if len(self.current) >= 3:
            self.contours.append(self.current)
        self.current = []

    _endPath = _closePath


def glyph_geometry(glyph_set, name):
    """A glyph's filled area in font units (y up), by the even-odd rule."""
    pen = FlatteningPen(glyph_set)
    glyph_set[name].draw(pen)
    area = Polygon()
    for contour in pen.contours:
        polygon = Polygon(contour).buffer(0)
        area = area.symmetric_difference(polygon)
    return area


def load_noto(path, weight):
    font = TTFont(path)
    return instancer.instantiateVariableFont(font, {"wght": weight}, inplace=False)


def text_geometry(font, text, height, centre):
    """`text` set in `font` with its cap height `height`, centred on `centre`, y down."""
    cmap = font.getBestCmap()
    glyph_set = font.getGlyphSet()
    cap = font["OS/2"].sCapHeight
    scale = height / cap
    x = 0.0
    shapes = []
    for char in text:
        name = cmap[ord(char)]
        shapes.append(affinity.translate(glyph_geometry(glyph_set, name), xoff=x))
        x += glyph_set[name].width
    shape = unary_union(shapes)
    minx, miny, maxx, maxy = shape.bounds
    shape = affinity.scale(shape, xfact=scale, yfact=-scale, origin=(0, 0))
    # Centred across on the ink, and on the cap height up and down.
    return affinity.translate(shape, xoff=centre[0] - (minx + maxx) * 0.5 * scale, yoff=centre[1] + cap * 0.5 * scale)


# ---------------------------------------------------------------------------------------------
# SVG writing


def write_icon(name, geometry, size=(100, 100)):
    path = OUT / "icons" / f"{name}.svg"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size[0]} {size[1]}">'
        f'<path fill="#fff" fill-rule="evenodd" d="{path_data(geometry)}"/></svg>\n',
        encoding="utf-8")


def unicode_attribute(char):
    return f"&#x{ord(char):X};"


def write_svg_font(path, family, units_per_em, ascent, descent, cap_height, glyphs, comment=""):
    """glyphs: (char, advance, geometry in font units (y up) or its path data)."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>']
    if comment:
        lines.append(f"<!-- {comment} -->")
    lines.append('<svg xmlns="http://www.w3.org/2000/svg">')
    lines.append("<defs>")
    lines.append(f'<font id="{family}" horiz-adv-x="{fmt(units_per_em * 0.5)}">')
    lines.append(
        f'<font-face font-family="{family}" units-per-em="{units_per_em}" ascent="{fmt(ascent)}" '
        f'descent="{fmt(descent)}" cap-height="{fmt(cap_height)}"/>')
    lines.append(f'<missing-glyph horiz-adv-x="{fmt(units_per_em * 0.5)}"/>')
    for char, advance, geometry in glyphs:
        data = geometry if isinstance(geometry, str) else path_data(geometry) if geometry is not None else ""
        d = f' d="{data}"' if data else ""
        lines.append(f'<glyph unicode="{unicode_attribute(char)}" horiz-adv-x="{fmt(advance)}"{d}/>')
    lines.append("</font>")
    lines.append("</defs>")
    lines.append("</svg>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------------------------
# The meter's digits: thin strokes on a wide box with rounded outer corners, as GT7's speed and
# gear readouts. Designed y down in a box 100 tall, flipped to the font's y up when written.

METER_HEIGHT = 100.0
METER_STROKE = 13.0
METER_WIDTH = 128.0
METER_GAP = 20.0
METER_RADIUS = 15.0


def meter_glyphs():
    w = METER_STROKE
    c = w / 2
    L, R = c, METER_WIDTH - c
    T, B = c, METER_HEIGHT - c
    M = METER_HEIGHT / 2
    r = METER_RADIUS

    def line(points, sharp=(), closed=False):
        return stroke(fillet(points, r, sharp=sharp, closed=closed), w, closed=closed)

    glyphs = {
        "0": (METER_WIDTH, [line([(L, T), (R, T), (R, B), (L, B)], closed=True)]),
        "1": (64.0, [stroke([(10, T), (64 - c, T), (64 - c, METER_HEIGHT)], w)]),
        "2": (METER_WIDTH, [line([(0, T), (R, T), (R, M), (L, M), (L, B), (METER_WIDTH, B)])]),
        "3": (METER_WIDTH, [line([(0, T), (R, T), (R, B), (0, B)]), stroke([(0, M), (R, M)], w)]),
        "4": (METER_WIDTH, [line([(L, 0), (L, M), (METER_WIDTH, M)]), stroke([(R, 0), (R, METER_HEIGHT)], w)]),
        "5": (METER_WIDTH, [line([(METER_WIDTH, T), (L, T), (L, M), (R, M), (R, B), (0, B)])]),
        "6": (METER_WIDTH, [line([(METER_WIDTH, T), (L, T), (L, B), (R, B), (R, M), (L, M)])]),
        "7": (METER_WIDTH, [line([(0, T), (R, T), (R, METER_HEIGHT)])]),
        "8": (METER_WIDTH, [line([(L, T), (R, T), (R, M), (L, M)], closed=True),
                            line([(L, M), (R, M), (R, B), (L, B)], closed=True)]),
        "9": (METER_WIDTH, [line([(0, B), (R, B), (R, T), (L, T), (L, M), (R, M)])]),
        "N": (METER_WIDTH, [stroke([(L, METER_HEIGHT), (L, T), (R, B), (R, 0)], w)]),
        "R": (METER_WIDTH, [line([(L, METER_HEIGHT), (L, T), (R, T), (R, M), (L, M)], sharp=(1, 4)),
                            stroke([(METER_WIDTH * 0.45, M), (R, METER_HEIGHT)], w)]),
        "-": (80.0, [stroke([(10, M), (70, M)], w)]),
        ".": (36.0, [Polygon([(6, B - c), (6 + w, B - c), (6 + w, METER_HEIGHT), (6, METER_HEIGHT)])]),
        ":": (36.0, [Polygon([(6, 26), (6 + w, 26), (6 + w, 26 + w), (6, 26 + w)]),
                     Polygon([(6, B - c), (6 + w, B - c), (6 + w, METER_HEIGHT), (6, METER_HEIGHT)])]),
        "'": (36.0, [stroke([(6 + c, 0), (6 + c, 28)], w)]),
        " ": (METER_WIDTH * 0.5, []),
    }
    result = []
    for char, (width, shapes) in glyphs.items():
        geometry = unary_union(shapes) if shapes else None
        if geometry is not None:
            # y down (0 at the top) to the font's y up with the baseline at 0.
            geometry = affinity.scale(geometry, xfact=1, yfact=-1, origin=(0, 0))
            geometry = affinity.translate(geometry, yoff=METER_HEIGHT)
        result.append((char, width + METER_GAP, geometry))
    return result


# ---------------------------------------------------------------------------------------------
# Icons, 100 units square, y down.

LAMP_STROKE = 6.5


def side_arcs(r=43.0, span=38.0):
    return unary_union([
        arc(50, 50, r, 180 - span, 180 + span, LAMP_STROKE),
        arc(50, 50, r, -span, span, LAMP_STROKE),
    ])


def make_icons(bold):
    s = LAMP_STROKE
    icons = {}

    icons["abs"] = unary_union([ring(50, 50, 30, s), side_arcs(), text_geometry(bold, "ABS", 15.5, (50, 50))])

    exclamation = unary_union([stroke([(50, 34), (50, 56)], 8.0), disc(50, 65, 4.6)])
    icons["handbrake"] = unary_union([ring(50, 50, 30, s), side_arcs(), exclamation])

    dumbbell = unary_union([disc(34, 50, 6.5), disc(66, 50, 6.5), stroke([(34, 50), (66, 50)], 4.0)])
    icons["brake_assist"] = unary_union([ring(50, 50, 30, s), side_arcs(), dumbbell])

    wheel = unary_union([
        ring(50, 50, 31, 7.5),
        disc(50, 54, 9),
        stroke([(19, 50), (81, 50)], 7.0),
        stroke([(50, 54), (50, 81)], 7.0),
    ])
    icons["steering"] = wheel

    headlamp = stroke(
        fillet([(54, 28), (64, 28), (82, 38), (82, 62), (64, 72), (54, 72)], 10, closed=True), s, closed=True)
    beams = unary_union([stroke([(16, y), (42, y)], 6.0) for y in (34, 45, 56, 67)])
    icons["headlight"] = unary_union([headlamp, stroke([(54, 26), (54, 74)], s), beams])

    cross = unary_union([
        ring(50, 50, 26, s),
        stroke([(50, 6), (50, 24)], s), stroke([(50, 76), (50, 94)], s),
        stroke([(6, 50), (24, 50)], s), stroke([(76, 50), (94, 50)], s),
        disc(50, 50, 8.5),
    ])
    icons["countersteer"] = cross

    # Traction control: a car from behind over two skid marks.
    body = Polygon(fillet([(20, 52), (24, 36), (32, 22), (68, 22), (76, 36), (80, 52), (80, 62), (20, 62)], 4, closed=True))
    window = Polygon(fillet([(31, 37), (37, 27), (63, 27), (69, 37)], 2, closed=True))
    lamps = unary_union([disc(29, 48, 3.6), disc(71, 48, 3.6)])
    wheels = unary_union([Polygon([(24, 60), (34, 60), (34, 69), (24, 69)]), Polygon([(66, 60), (76, 60), (76, 69), (66, 69)])])
    car = unary_union([body, wheels]).difference(window).difference(lamps)

    def skid(x0, direction):
        points = [(x0 + direction * 6 * math.sin(t / 22 * math.pi * 1.6), 72 + t) for t in range(0, 23)]
        return stroke(points, 5.0)

    icons["tcs"] = unary_union([car, skid(36, 1), skid(64, -1)])

    triangle = stroke(fillet([(50, 14), (88, 82), (12, 82)], 6, closed=True), 7.0, closed=True)
    icons["warning"] = unary_union([triangle, stroke([(50, 38), (50, 60)], 7.5), disc(50, 70, 4.4)])

    # Throttle: a pedal pressed down between two brackets.
    icons["throttle"] = unary_union([
        arc(-16, 50, 38, -42, 42, 7.0),
        arc(116, 50, 38, 138, 222, 7.0),
        stroke([(38, 30), (54, 52)], 6.5, cap="round"),
        Polygon([(62, 66), (44, 58), (58, 46)]),
    ])

    # Surface water: a tyre's profile, its sidewalls bulging, with a drop between them.
    walls = unary_union([arc(66, 50, 36, 140, 220, 7.0), arc(34, 50, 36, -40, 40, 7.0)])
    drop = Polygon([(50, 30)] + arc_points(50, 58, 12, -25, 205, 8)).buffer(0)
    icons["water"] = unary_union([walls, drop, stroke([(22, 80), (78, 80)], 6.0)])

    # Fuel pump.
    pump_body = stroke(fillet([(24, 22), (54, 22), (54, 80), (24, 80)], 5, closed=True), 6.0, closed=True)
    pump_window = Polygon([(30, 30), (48, 30), (48, 44), (30, 44)])
    hose = stroke(fillet([(54, 36), (64, 42), (64, 70), (72, 70), (72, 36), (64, 26)], 4), 5.0)
    icons["fuel"] = unary_union([pump_body, pump_window, hose, stroke([(18, 82), (60, 82)], 5.0)])

    # Turbo: a compressor wheel in its scroll, the outlet to the right.
    blades = unary_union([
        stroke(arc_points(50, 56, 12, a, a + 70), 4.5) for a in (0, 90, 180, 270)
    ])
    scroll = arc(50, 56, 24, 300, 600, 6.0)
    outlet = stroke([(50, 32), (84, 32)], 6.0)
    icons["turbo"] = unary_union([scroll, outlet, blades, disc(50, 56, 4.5), stroke([(84, 24), (84, 40)], 5.0)])

    # Arrows beside the meter panel.
    arrow = Polygon([(8, 50), (50, 14), (50, 34), (92, 34), (92, 66), (50, 66), (50, 86)])
    icons["arrow_left"] = arrow
    icons["arrow_right"] = affinity.scale(arrow, xfact=-1, yfact=1, origin=(50, 50))

    for name, geometry in icons.items():
        write_icon(name, geometry)

    # The tyre widget's car seen from above (60 x 120), in two tones: the body filled dark, then its
    # outline, bumpers and roof light; and a marker at each corner.
    outline_points = fillet([(16, 8), (44, 8), (52, 22), (52, 108), (44, 116), (16, 116), (8, 108), (8, 22)], 9, closed=True)
    shell = Polygon(outline_points)
    write_icon("car_top", shell, (60, 120))
    roof = Polygon(fillet([(17, 44), (43, 44), (43, 82), (17, 82)], 6, closed=True))
    windscreen = stroke(fillet([(14, 40), (20, 30), (40, 30), (46, 40)], 4), 3.5)
    rear_window = stroke(fillet([(14, 86), (20, 94), (40, 94), (46, 86)], 4), 3.5)
    bumpers = unary_union([arc(30, 30, 20, 205, 335, 3.5), arc(30, 96, 20, 25, 155, 3.5)])
    lines = unary_union([stroke(outline_points, 3.5, closed=True), roof, windscreen, rear_window, bumpers])
    write_icon("car_lines", lines.intersection(shell.buffer(1.75)), (60, 120))
    markers = unary_union([
        Polygon([(0, 22), (6, 18), (6, 26)]), Polygon([(60, 22), (54, 18), (54, 26)]),
        Polygon([(0, 80), (6, 76), (6, 84)]), Polygon([(60, 80), (54, 76), (54, 84)]),
    ])
    write_icon("car_markers", markers, (60, 120))


# ---------------------------------------------------------------------------------------------


def write_sans_font(font, path, family, chars, comment):
    cmap = font.getBestCmap()
    glyph_set = font.getGlyphSet()
    glyphs = []
    for char in chars:
        name = cmap.get(ord(char))
        if name is None:
            raise SystemExit(f"{family}: no glyph for {char!r}")
        # The outline as the font has it (quadratic curves), filled by the nonzero rule as TrueType is.
        pen = SVGPathPen(glyph_set, ntos=fmt)
        glyph_set[name].draw(pen)
        glyphs.append((char, glyph_set[name].width, pen.getCommands()))
    hhea = font["hhea"]
    write_svg_font(path, family, font["head"].unitsPerEm, hhea.ascent, -hhea.descent, font["OS/2"].sCapHeight, glyphs, comment)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--noto", default="C:/Windows/Fonts/NotoSansSC-VF.ttf", help="Noto Sans SC (variable)")
    args = parser.parse_args()

    regular = load_noto(args.noto, 350)
    bold = load_noto(args.noto, 700)
    notice = ("Derived from Noto Sans SC, (c) 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name "
              "'Source'; licensed under the SIL Open Font License 1.1 (OFL.txt). Written by "
              "tools/gt7_hud/make_gt7_hud_svgs.py.")
    ascii_chars = "".join(chr(c) for c in range(32, 127))
    labels = "".join(dict.fromkeys(CJK_LABELS))
    write_sans_font(regular, OUT / "fonts" / "gt7_sans.svg", "GT7HudSans", ascii_chars + labels.replace("/", ""), notice)
    bold_chars = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    write_sans_font(bold, OUT / "fonts" / "gt7_sans_bold.svg", "GT7HudSansBold", bold_chars, notice)
    write_svg_font(
        OUT / "fonts" / "gt7_meter.svg", "GT7HudMeter", int(METER_HEIGHT), METER_HEIGHT, 0, METER_HEIGHT,
        meter_glyphs(), "The meter's digits, drawn by tools/gt7_hud/make_gt7_hud_svgs.py.")
    make_icons(bold)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
