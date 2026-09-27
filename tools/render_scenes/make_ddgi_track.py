#!/usr/bin/env python3
"""Writes the DDGI outdoor test track (tests/fixtures/render_scenes/models/ddgi_track/).

A straight road running away from the default camera (-Z), grass to both sides, a 60 m concrete tunnel
over the road, a grandstand with a roof on the left, a row of trees made of crossed leaf cards (an
alpha-masked texture, double-sided) on the right and a car on the road. Metres, Y up, the ground at
y = 0; the scene lowers it 1.5 m under the camera.

    python3 tools/render_scenes/make_ddgi_track.py
"""

import base64
import json
import os
import random
import struct
import zlib

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "fixtures", "render_scenes", "models", "ddgi_track")


class Mesh:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []

    def quad(self, a, b, c, d, normal, uv_scale=(1.0, 1.0)):
        """Counter-clockwise a, b, c, d seen from the normal's side."""
        base = len(self.positions)
        self.positions += [a, b, c, d]
        self.normals += [normal] * 4
        su, sv = uv_scale
        self.uvs += [(0.0, sv), (su, sv), (su, 0.0), (0.0, 0.0)]
        self.indices += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        self.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), (0, 1, 0))
        self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0))
        self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1))
        self.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (0, 0, -1))
        self.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (1, 0, 0))
        self.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (-1, 0, 0))


def leaves_png(path):
    """64 x 64 RGBA: green blobs, transparent between them."""
    random.seed(7)
    size = 64
    blobs = [(random.uniform(0, size), random.uniform(0, size), random.uniform(3, 7)) for _ in range(70)]
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            inside = any((x - bx) ** 2 + (y - by) ** 2 < r * r for bx, by, r in blobs)
            shade = 0.7 + 0.3 * ((x * 7 + y * 13) % 11) / 10.0
            row += bytes([int(40 * shade), int(95 * shade), int(25 * shade), 255 if inside else 0])
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        file.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)))
        file.write(chunk(b"IDAT", raw))
        file.write(chunk(b"IEND", b""))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    meshes = {}

    grass = Mesh()
    grass.quad((-200, 0, 60), (200, 0, 60), (200, 0, -360), (-200, 0, -360), (0, 1, 0), (40, 40))
    meshes["grass"] = grass

    road = Mesh()
    road.quad((-6, 0.02, 60), (6, 0.02, 60), (6, 0.02, -360), (-6, 0.02, -360), (0, 1, 0), (1, 40))
    meshes["asphalt"] = road

    # The tunnel: two walls and a roof, 1 m thick, over the road from z = -80 to -140.
    concrete = Mesh()
    concrete.box((-9, 0, -140), (-8, 7, -80))
    concrete.box((8, 0, -140), (9, 7, -80))
    concrete.box((-9, 7, -140), (9, 8, -80))
    # The grandstand on the left: ten steps rising away from the road, a back wall.
    for step in range(10):
        concrete.box((-30 - step * 1.0, 0, -60), (-20 - step * 1.0 + 1.0, 0.6 * (step + 1), 0))
    concrete.box((-31, 0, -60), (-30, 12, 0))
    # Its roof's columns.
    for z in (-58, -30, -2):
        concrete.box((-20.5, 0, z - 0.3), (-19.9, 12, z + 0.3))
    meshes["concrete"] = concrete

    roof = Mesh()
    roof.box((-31, 12, -60), (-18, 12.4, 0))
    meshes["roof"] = roof

    # Trees on the right: a trunk and two crossed leaf cards each, every 12 m.
    bark = Mesh()
    leaves = Mesh()
    for index in range(20):
        z = 10 - index * 12
        x = 16 + (index % 3) * 2.5
        bark.box((x - 0.2, 0, z - 0.2), (x + 0.2, 4, z + 0.2))
        leaves.quad((x - 3, 2.5, z), (x + 3, 2.5, z), (x + 3, 9.5, z), (x - 3, 9.5, z), (0, 0, 1))
        leaves.quad((x, 2.5, z + 3), (x, 2.5, z - 3), (x, 9.5, z - 3), (x, 9.5, z + 3), (1, 0, 0))
    meshes["bark"] = bark
    meshes["leaves"] = leaves

    car = Mesh()
    car.box((-2.5, 0.3, -24.5), (-0.7, 1.1, -20))
    car.box((-2.3, 1.1, -23.5), (-0.9, 1.5, -21.2))
    meshes["car_paint"] = car

    materials = {
        "grass": {"color": [0.10, 0.18, 0.05, 1.0], "roughness": 0.95},
        "asphalt": {"color": [0.07, 0.07, 0.07, 1.0], "roughness": 0.85},
        "concrete": {"color": [0.45, 0.44, 0.42, 1.0], "roughness": 0.9},
        "roof": {"color": [0.35, 0.36, 0.40, 1.0], "roughness": 0.6},
        "bark": {"color": [0.12, 0.08, 0.05, 1.0], "roughness": 0.9},
        "leaves": {"color": [1.0, 1.0, 1.0, 1.0], "roughness": 0.8, "texture": True},
        "car_paint": {"color": [0.55, 0.03, 0.03, 1.0], "roughness": 0.3},
    }

    buffer = bytearray()
    accessors = []
    buffer_views = []

    def add_view(data, target):
        while len(buffer) % 4:
            buffer.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "target": target})
        buffer.extend(data)
        return len(buffer_views) - 1

    gltf_meshes = []
    gltf_materials = []
    nodes = []
    for name, mesh in meshes.items():
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
        spec = materials[name]
        material = {
            "name": name,
            "pbrMetallicRoughness": {"baseColorFactor": spec["color"], "metallicFactor": 0.0, "roughnessFactor": spec["roughness"]},
        }
        if spec.get("texture"):
            material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = 0.5
            material["doubleSided"] = True
        gltf_materials.append(material)
        gltf_meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2}, "indices": base + 3, "material": len(gltf_materials) - 1}]})
        nodes.append({"mesh": len(gltf_meshes) - 1, "name": name})

    leaves_png(os.path.join(OUT_DIR, "leaves.png"))
    gltf = {
        "asset": {"version": "2.0", "generator": "tools/render_scenes/make_ddgi_track.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": gltf_meshes,
        "materials": gltf_materials,
        "textures": [{"source": 0}],
        "images": [{"uri": "leaves.png"}],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    with open(os.path.join(OUT_DIR, "ddgi_track.gltf"), "w") as file:
        json.dump(gltf, file)
    print("wrote", os.path.normpath(OUT_DIR))


if __name__ == "__main__":
    main()
