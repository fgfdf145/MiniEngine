#!/usr/bin/env python3
"""A top-down picture of a built level's collision: the highest collision surface under every pixel,
shaded by height and slope, read back from the glTF (so it checks the written file, not the converter's
memory). For finding test strips and placing cars without the engine.

    python tools/beamng/bng_heightmap.py <model.gltf> x0 y0 x1 y1 [--pixel 0.1] [--out map.png]

x / y are BeamNG's (east, north); the picture has north up. Prints the height under given points with
--probe x,y (repeatable).
"""

import argparse
import json
import os

import numpy as np
from PIL import Image


def read_collision(path):
    with open(path) as file:
        document = json.load(file)
    with open(os.path.join(os.path.dirname(path), document["buffers"][0]["uri"]), "rb") as file:
        blob = file.read()

    def accessor(index, dtype, width):
        a = document["accessors"][index]
        view = document["bufferViews"][a["bufferView"]]
        offset = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        return np.frombuffer(blob, dtype=dtype, count=a["count"] * width, offset=offset).reshape(-1, width) if width > 1 else \
            np.frombuffer(blob, dtype=dtype, count=a["count"], offset=offset)

    surfaces = []
    for node in document["nodes"]:
        extension = node.get("extensions", {}).get("MINIENGINE_collision")
        if extension is None:
            continue
        primitive = document["meshes"][node["mesh"]]["primitives"][0]
        positions = accessor(primitive["attributes"]["POSITION"], np.float32, 3).astype(np.float64)
        indices = accessor(primitive["indices"], np.uint32, 1).astype(np.int64)
        surfaces.append((extension["surface"], positions, indices.reshape(-1, 3)))
    return surfaces


def rasterize(surfaces, x0, y0, x1, y1, pixel):
    width, height = int(round((x1 - x0) / pixel)), int(round((y1 - y0) / pixel))
    top = np.full((height, width), -np.inf)
    surface_id = np.full((height, width), -1, dtype=np.int32)
    for sid, (_, positions, triangles) in enumerate(surfaces):
        tri = positions[triangles]  # (n, 3, 3)
        lo, hi = tri.min(1), tri.max(1)
        keep = (hi[:, 0] >= x0) & (lo[:, 0] <= x1) & (hi[:, 1] >= y0) & (lo[:, 1] <= y1)
        for a, b, c in tri[keep]:
            # Pixel centres inside the triangle's box.
            i0 = max(int(np.floor((min(a[0], b[0], c[0]) - x0) / pixel)), 0)
            i1 = min(int(np.ceil((max(a[0], b[0], c[0]) - x0) / pixel)), width)
            j0 = max(int(np.floor((y1 - max(a[1], b[1], c[1])) / pixel)), 0)
            j1 = min(int(np.ceil((y1 - min(a[1], b[1], c[1])) / pixel)), height)
            if i1 <= i0 or j1 <= j0:
                continue
            px = x0 + (np.arange(i0, i1) + 0.5) * pixel
            py = y1 - (np.arange(j0, j1) + 0.5) * pixel
            X, Y = np.meshgrid(px, py)
            d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(d) < 1e-12:
                continue
            l1 = ((b[1] - c[1]) * (X - c[0]) + (c[0] - b[0]) * (Y - c[1])) / d
            l2 = ((c[1] - a[1]) * (X - c[0]) + (a[0] - c[0]) * (Y - c[1])) / d
            l3 = 1 - l1 - l2
            inside = (l1 >= -1e-9) & (l2 >= -1e-9) & (l3 >= -1e-9)
            z = l1 * a[2] + l2 * b[2] + l3 * c[2]
            block = top[j0:j1, i0:i1]
            better = inside & (z > block)
            block[better] = z[better]
            surface_id[j0:j1, i0:i1][better] = sid
    return top, surface_id


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("gltf")
    parser.add_argument("bounds", nargs=4, type=float)
    parser.add_argument("--pixel", type=float, default=0.1)
    parser.add_argument("--out", default="collision_map.png")
    parser.add_argument("--probe", action="append", default=[])
    args = parser.parse_args()
    x0, y0, x1, y1 = args.bounds
    surfaces = read_collision(args.gltf)
    top, ids = rasterize(surfaces, x0, y0, x1, y1, args.pixel)
    known = np.isfinite(top)
    base = np.median(top[known]) if known.any() else 0.0
    gy, gx = np.gradient(np.where(known, top, base), args.pixel)
    shade = np.clip(0.75 + 0.25 * (-gx - gy) / np.sqrt(1 + gx * gx + gy * gy), 0, 1)
    relief = np.clip((top - base) / 0.5, -1, 1)  # +-0.5 m around the median
    rgb = np.stack([0.5 + 0.5 * np.clip(relief, 0, 1), 0.5 - 0.25 * np.abs(relief), 0.5 + 0.5 * np.clip(-relief, 0, 1)], -1)
    rgb *= shade[..., None]
    rgb[~known] = 0
    Image.fromarray((rgb * 255).astype(np.uint8)).save(args.out)
    names = [s[0] for s in surfaces]
    print("median height {:.3f}; surfaces {}".format(base, {n: int((ids == k).sum()) for k, n in enumerate(names) if (ids == k).any()}))
    for probe in args.probe:
        x, y = (float(v) for v in probe.split(","))
        i, j = int((x - x0) / args.pixel), int((y1 - y) / args.pixel)
        print("height at ({}, {}): {:.4f} on {}".format(x, y, top[j, i], names[ids[j, i]] if ids[j, i] >= 0 else None))


if __name__ == "__main__":
    main()
