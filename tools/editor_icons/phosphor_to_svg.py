#!/usr/bin/env python3
"""Writes the Assets window's type icons as SVG files, taken from Phosphor's regular weight (the
icons of the Claude desktop app), from the assets of the @phosphor-icons/core npm package.

    npm pack @phosphor-icons/core && tar xzf phosphor-icons-core-*.tgz
    python tools/editor_icons/phosphor_to_svg.py package

Each icon becomes engine/asset/icons/<name>.svg, Phosphor's own file unchanged: a 256 x 256 viewBox
with one filled path. The engine draws them as vectors (engine/asset/svg_icon.h), so they stay sharp
at any size."""

import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUTPUT = REPO / "engine" / "asset" / "icons"

# File name -> Phosphor icon name.
ICONS = {
    "folder": "folder",
    "model": "cube",
    "material": "palette",
    "scene": "mountains",
    "texture": "image",
    "file": "file",
    "parent_folder": "arrow-bend-left-up",
    "audio": "music-notes",
}


def main():
    package = Path(sys.argv[1] if len(sys.argv) > 1 else "package")
    regular = package / "assets" / "regular"
    for name, icon in ICONS.items():
        shutil.copyfile(regular / f"{icon}.svg", OUTPUT / f"{name}.svg")
        print(f"{icon} -> {OUTPUT / (name + '.svg')}")


if __name__ == "__main__":
    main()
