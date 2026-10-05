#!/usr/bin/env python3
"""Writes the Assets window's type icons as SVG files, taken from the Font Awesome font in
third_party/fontawesome (needs fontTools: pip install fonttools).

    python tools/editor_icons/fontawesome_to_svg.py

Each icon becomes engine/asset/icons/<name>.svg, laid out as Font Awesome's own SVGs are: a
viewBox the glyph's advance wide and 512 tall, the baseline at y = 448. The engine draws them as
vectors (engine/asset/svg_icon.h), so they stay sharp at any size."""

from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

REPO = Path(__file__).resolve().parents[2]
FONT = REPO / "third_party" / "fontawesome" / "fa-solid-900.ttf"
OUTPUT = REPO / "engine" / "asset" / "icons"

# File name -> Font Awesome glyph name.
ICONS = {
    "folder": "folder",
    "model": "cube",
    "material": "palette",
    "scene": "mountain-sun",
    "texture": "image",
    "file": "file",
    "parent_folder": "arrow-turn-up",
}

BASELINE = 448  # Font Awesome's SVGs put the font's baseline here, in a 512-unit em


def number(value):
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return "0" if text == "-0" else text


def main():
    font = TTFont(FONT)
    glyphs = font.getGlyphSet()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, glyph_name in ICONS.items():
        width = font["hmtx"][glyph_name][0]
        path_pen = SVGPathPen(glyphs, ntos=number)
        # Font units point y up; SVG's y points down from the top of the em.
        glyphs[glyph_name].draw(TransformPen(path_pen, (1, 0, 0, -1, 0, BASELINE)))
        svg = (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 512">'
            f"<!-- Font Awesome Free {glyph_name} by @fontawesome - https://fontawesome.com "
            f"License - https://fontawesome.com/license/free (Icons: CC BY 4.0) -->"
            f'<path d="{path_pen.getCommands()}"/></svg>\n'
        )
        (OUTPUT / f"{name}.svg").write_text(svg, encoding="utf-8", newline="\n")
        print(f"{name}.svg <- {glyph_name}")


if __name__ == "__main__":
    main()
