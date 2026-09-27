#!/usr/bin/env python3
"""Writes the DDGI Cornell box (tests/fixtures/render_scenes/models/cornell_box/).

A 4 m box open at the front (+Z): a white floor, ceiling and back wall, a red wall on the left and a
green one on the right, a tall and a short white block inside, and a 1.2 m square hole in the ceiling
that lets the sun in. The walls are 0.1 m thick, so shadow maps and rays see solid slabs. Metres, Y up,
the floor's top at y = 0, the opening at z = 0.

    python3 tools/render_scenes/make_cornell_box.py
"""

import math
import os

from make_ddgi_track import Mesh, write_gltf

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "fixtures", "render_scenes", "models", "cornell_box")

SIZE = 4.0
THICK = 0.1
HOLE = 1.2


def turned_box(mesh, centre, size, degrees):
    """A box of the given size standing on y = centre[1] - size[1] / 2, turned about Y."""
    cx, cy, cz = centre
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    angle = math.radians(degrees)
    c, s = math.cos(angle), math.sin(angle)

    def point(x, y, z):
        return (cx + c * x + s * z, cy + y, cz - s * x + c * z)

    def normal(x, y, z):
        return (c * x + s * z, y, -s * x + c * z)

    mesh.quad(point(-hx, hy, hz), point(hx, hy, hz), point(hx, hy, -hz), point(-hx, hy, -hz), normal(0, 1, 0))
    mesh.quad(point(-hx, -hy, -hz), point(hx, -hy, -hz), point(hx, -hy, hz), point(-hx, -hy, hz), normal(0, -1, 0))
    mesh.quad(point(-hx, -hy, hz), point(hx, -hy, hz), point(hx, hy, hz), point(-hx, hy, hz), normal(0, 0, 1))
    mesh.quad(point(hx, -hy, -hz), point(-hx, -hy, -hz), point(-hx, hy, -hz), point(hx, hy, -hz), normal(0, 0, -1))
    mesh.quad(point(hx, -hy, hz), point(hx, -hy, -hz), point(hx, hy, -hz), point(hx, hy, hz), normal(1, 0, 0))
    mesh.quad(point(-hx, -hy, -hz), point(-hx, -hy, hz), point(-hx, hy, hz), point(-hx, hy, -hz), normal(-1, 0, 0))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    half = SIZE / 2

    white = Mesh()
    # Floor, back wall.
    white.box((-half - THICK, -THICK, -SIZE - THICK), (half + THICK, 0.0, 0.0))
    white.box((-half - THICK, 0.0, -SIZE - THICK), (half + THICK, SIZE, -SIZE))
    # Ceiling around the hole, which sits over the middle of the floor.
    hole = HOLE / 2
    z_mid = -half
    top = (SIZE, SIZE + THICK)
    white.box((-half - THICK, top[0], -SIZE - THICK), (half + THICK, top[1], z_mid - hole))
    white.box((-half - THICK, top[0], z_mid + hole), (half + THICK, top[1], 0.0))
    white.box((-half - THICK, top[0], z_mid - hole), (-hole, top[1], z_mid + hole))
    white.box((hole, top[0], z_mid - hole), (half + THICK, top[1], z_mid + hole))
    # The blocks: a tall one at the back left, a short one at the front right.
    turned_box(white, (-0.7, 1.2, -2.7), (1.2, 2.4, 1.2), 17.0)
    turned_box(white, (0.75, 0.6, -1.3), (1.2, 1.2, 1.2), -18.0)

    red = Mesh()
    red.box((-half - THICK, 0.0, -SIZE), (-half, SIZE, 0.0))
    green = Mesh()
    green.box((half, 0.0, -SIZE), (half + THICK, SIZE, 0.0))

    meshes = {"white": white, "red": red, "green": green}
    # The classic box's reflectances, rounded.
    materials = {
        "white": {"color": [0.73, 0.73, 0.73, 1.0], "roughness": 1.0},
        "red": {"color": [0.63, 0.065, 0.05, 1.0], "roughness": 1.0},
        "green": {"color": [0.14, 0.45, 0.091, 1.0], "roughness": 1.0},
    }
    write_gltf(OUT_DIR, "cornell_box", meshes, materials, [], "tools/render_scenes/make_cornell_box.py")
    print("wrote", os.path.normpath(OUT_DIR))


if __name__ == "__main__":
    main()
