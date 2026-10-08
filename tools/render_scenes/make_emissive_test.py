#!/usr/bin/env python3
"""Writes the emissive surfaces test set (tests/fixtures/render_scenes/models/emissive_test/), lit by
nothing but its emissive materials, for the path tracer's emissive triangle NEE (scene:
assets/scenes/emissive_test.yaml).

A dark 30 m concrete yard with a back wall (z = -8) and a left wall (x = -10), open to the right and to
the front, a glossy wet patch in front of the middle (where a car stands in the scene), a chrome and a
white sphere, and the emitters, each a case the light list and its sampling must get right:

- Cyan panel: 3 x 1 m on the back wall, 400 nit, double-sided (a sign).
- Striped panel: 2 x 1 m on the back wall, 600 nit times an emissive texture whose every other 8-texel
  column is black (a power estimate from the texture, half the area emitting nothing).
- Pink tubes: four 6 cm thick, 2.4 m tall bars by the left wall, 3000 nit (thin, bright neon).
- Bulb: a 5 cm sphere 3 m up on a post, 50000 nit warm white (small and very bright: fireflies).
- Soft box: 3 x 2 m on the left wall, 20 nit (large and dim).
- RGB cubes: three 0.4 m cubes by the back wall, red, green and blue, 150 nit (coloured bounce).
- One-sided panel: 1.2 x 0.6 m, 800 nit orange, facing the back wall, single-sided: from the front of
  the yard only its dark back is seen, and its light must only reach the wall side.

Metres, Y up, the ground's top at y = 0. Luminance in nit is emissiveFactor x
KHR_materials_emissive_strength, as the engine reads it.

    python3 tools/render_scenes/make_emissive_test.py
"""

import base64
import json
import os
import struct
import zlib

from make_ddgi_track import Mesh

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "fixtures", "render_scenes", "models", "emissive_test")
NAME = "emissive_test"
MODEL_UUID = "e1155e00-7e57-4c0d-9a1e-000000000001"
STRIPES_UUID = "e1155e00-7e57-4c0d-9a1e-000000000002"


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def scale(a, s):
    return tuple(x * s for x in a)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def panel(mesh, centre, right, up, width, height):
    """A width x height quad facing right x up."""
    r = scale(right, width / 2)
    u = scale(up, height / 2)
    a = add(centre, scale(add(r, u), -1))
    b = add(add(centre, r), scale(u, -1))
    c = add(add(centre, r), u)
    d = add(add(centre, scale(r, -1)), u)
    mesh.quad(a, b, c, d, cross(right, up))


def bar(mesh, centre, size):
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    mesh.box((centre[0] - hx, centre[1] - hy, centre[2] - hz), (centre[0] + hx, centre[1] + hy, centre[2] + hz))


def stripes_png(path):
    """64 x 64 RGB: white in every other 8-texel column, black between."""
    size = 64
    rows = []
    for _ in range(size):
        row = bytearray([0])
        for x in range(size):
            row += bytes([255, 255, 255] if (x // 8) % 2 == 0 else [0, 0, 0])
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        file.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)))
        file.write(chunk(b"IDAT", zlib.compress(b"".join(rows), 9)))
        file.write(chunk(b"IEND", b""))


def build():
    """Name -> (Mesh, material) for every part."""
    parts = {}

    def part(name, material):
        mesh = Mesh()
        parts[name] = (mesh, dict(material, name=name))
        return mesh

    def surface(color, roughness, metallic=0.0):
        return {"pbrMetallicRoughness": {"baseColorFactor": list(color) + [1.0], "metallicFactor": metallic, "roughnessFactor": roughness}}

    def emitter(color, nits, double_sided=True, texture=None):
        material = surface((0.05, 0.05, 0.05), 0.5)
        material["emissiveFactor"] = list(color)
        material["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": nits}}
        material["doubleSided"] = double_sided
        if texture is not None:
            material["emissiveTexture"] = {"index": texture}
        return material

    x_axis, y_axis, z_axis = (1, 0, 0), (0, 1, 0), (0, 0, 1)

    # The yard: ground, walls, the wet patch, the post under the bulb.
    part("Ground", surface((0.25, 0.25, 0.24), 0.7)).box((-15, -0.2, -15), (15, 0, 15))
    walls = part("Walls", surface((0.5, 0.5, 0.48), 0.8))
    walls.box((-10, 0, -8.3), (10, 5, -8))
    walls.box((-10.3, 0, -8), (-10, 5, 6))
    part("Wet patch", surface((0.03, 0.03, 0.03), 0.06)).box((-4, 0, 2.6), (4, 0.004, 6))
    part("Lamp post", surface((0.08, 0.08, 0.08), 0.4, 1.0)).box((5.97, 0, -3.03), (6.03, 2.95, -2.97))

    # Probes for what the emitters light and how they look in a mirror.
    part("Chrome sphere", surface((0.95, 0.95, 0.95), 0.05, 1.0)).sphere((-5.5, 0.5, 1.0), 0.5)
    part("White sphere", surface((0.8, 0.8, 0.8), 0.6)).sphere((4.5, 0.5, -1.5), 0.5)

    # The emitters.
    panel(part("Cyan panel", emitter((0.1, 0.8, 1.0), 400)), (-3, 2.5, -7.98), x_axis, y_axis, 3.0, 1.0)
    panel(part("Striped panel", emitter((1.0, 0.8, 0.3), 600, texture=0)), (3, 2.5, -7.98), x_axis, y_axis, 2.0, 1.0)
    tubes = part("Pink tubes", emitter((1.0, 0.1, 0.6), 3000))
    for index in range(4):
        bar(tubes, (-9.2, 1.2, -5.0 + index * 1.2), (0.06, 2.4, 0.06))
    part("Bulb", emitter((1.0, 0.75, 0.45), 50000)).sphere((6, 3.0, -3), 0.05, rings=12, segments=24)
    panel(part("Soft box", emitter((1.0, 1.0, 1.0), 20)), (-9.98, 2.5, 1.5), (0, 0, -1), y_axis, 3.0, 2.0)
    for index, color in enumerate([(1.0, 0.05, 0.05), (0.05, 1.0, 0.05), (0.05, 0.1, 1.0)]):
        bar(part("RGB cube " + "RGB"[index], emitter(color, 150)), (7.4 + index * 0.6, 0.2, -5.0), (0.4, 0.4, 0.4))
    panel(part("One-sided panel", emitter((1.0, 0.45, 0.1), 800, double_sided=False)), (6, 1.2, 3.0), (-1, 0, 0), y_axis, 1.2, 0.6)
    return parts


def write_gltf(parts):
    buffer = bytearray()
    accessors = []
    buffer_views = []

    def add_view(data, target):
        while len(buffer) % 4:
            buffer.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "target": target})
        buffer.extend(data)
        return len(buffer_views) - 1

    meshes, materials, nodes = [], [], []
    for name, (mesh, material) in parts.items():
        positions = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
        normals = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
        uvs = b"".join(struct.pack("<2f", *u) for u in mesh.uvs)
        indices = b"".join(struct.pack("<I", i) for i in mesh.indices)
        lo = [min(p[axis] for p in mesh.positions) for axis in range(3)]
        hi = [max(p[axis] for p in mesh.positions) for axis in range(3)]
        accessors.append({"bufferView": add_view(positions, 34962), "componentType": 5126, "count": len(mesh.positions), "type": "VEC3", "min": lo, "max": hi})
        accessors.append({"bufferView": add_view(normals, 34962), "componentType": 5126, "count": len(mesh.normals), "type": "VEC3"})
        accessors.append({"bufferView": add_view(uvs, 34962), "componentType": 5126, "count": len(mesh.uvs), "type": "VEC2"})
        accessors.append({"bufferView": add_view(indices, 34963), "componentType": 5125, "count": len(mesh.indices), "type": "SCALAR"})
        base = len(accessors) - 4
        materials.append(material)
        meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2}, "indices": base + 3, "material": len(materials) - 1}]})
        nodes.append({"mesh": len(meshes) - 1, "name": name})

    gltf = {
        "asset": {"version": "2.0", "generator": "tools/render_scenes/make_emissive_test.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "textures": [{"source": 0}],
        "images": [{"uri": "textures/stripes.png"}],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    with open(os.path.join(OUT_DIR, NAME + ".gltf"), "w") as file:
        json.dump(gltf, file)


def write_sidecar(path, uuid):
    with open(path + ".miniengine_asset.yaml", "w", newline="\n") as file:
        file.write("asset:\n  uuid: %s\n  file: %s" % (uuid, os.path.basename(path)))


def main():
    os.makedirs(os.path.join(OUT_DIR, "textures"), exist_ok=True)
    stripes = os.path.join(OUT_DIR, "textures", "stripes.png")
    stripes_png(stripes)
    write_sidecar(stripes, STRIPES_UUID)
    parts = build()
    write_gltf(parts)
    write_sidecar(os.path.join(OUT_DIR, NAME + ".gltf"), MODEL_UUID)
    triangles = sum(len(mesh.indices) // 3 for mesh, material in parts.values() if "emissiveFactor" in material)
    print("%d parts, %d emissive triangles -> %s" % (len(parts), triangles, os.path.normpath(OUT_DIR)))


if __name__ == "__main__":
    main()
