#!/usr/bin/env python3
"""Writes a white photo studio for showing a car and judging rendering: an infinity cove
(tests/fixtures/render_scenes/models/studio_cove/) and a scene that stands the Skyline R34 in it under
soft box lights (assets/scenes/vehicle_studio.yaml).

The cove is the usual cyclorama: a back wall and two side walls that curve into the floor through a
COVE_RADIUS fillet, with no edge anywhere for the eye (or a reflection) to catch, open at the front for
the camera and open above like a studio's dark grid. In plan the walls form a U whose back corners are
rounded too (PLAN_RADIUS), so the fillet sweeps round them without a seam. Vertex colours shade it from
a light grey floor to white walls across the fillet, as studio floors scuffed by cars read greyer than
their walls; one material, so the shading has no seam either.

The lights are one-sided area lights (rectangles, lumens), the soft boxes of a car shoot: a large
overhead box over the car (the long soft highlight down the bonnet and roof), a key box front-right
and a weaker fill front-left (the side toward the camera and the nose), and a level panel facing down
over the back of the cove, washing the backdrop to white behind the car. That panel hangs above the
walls' top on purpose: a one-sided light lights nothing behind its plane, and where that plane cuts a
wall the backdrop, exposed far past white, shows the cut as a hard line. The open top and front see a
dim grey Ambient light, the studio's ceiling and camera side; the DDGI probes carry the white walls'
bounce, which is most of the light in a real cove.

The auto exposure would meter the white cove to a middle grey, so the scene carries
EXPOSURE_COMPENSATION_EV (environment.exposure_compensation_ev), added to the camera's compensation.

Metres, Y up, the floor at y = 0, the car at the origin facing +Z, the camera in front (+Z) and to the
right looking back, so the nose points to the picture's left as in a maker's press photo. Run from the
repository's root, then scripts/install-render-scenes.* to copy the model into assets/models/:

    python3 tools/render_scenes/make_vehicle_studio.py
"""

import base64
import json
import math
import os
import struct

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
OUT_DIR = os.path.join(ROOT, "tests", "fixtures", "render_scenes", "models", "studio_cove")
SCENE = os.path.join(ROOT, "assets", "scenes", "vehicle_studio.yaml")
NAME = "studio_cove"
MODEL_UUID = "57d10c0e-1a2b-4c3d-8e4f-000000000001"

HALF_WIDTH = 11.0  # side walls at x = +-HALF_WIDTH
BACK_Z = -10.0  # back wall
FRONT_Z = 10.0  # where the side walls and the floor end, open toward the camera
HEIGHT = 10.0  # walls' top
COVE_RADIUS = 3.0  # the floor-to-wall fillet
PLAN_RADIUS = 5.0  # the back corners in plan; the floor's corner radius is PLAN_RADIUS - COVE_RADIUS
FLOOR_GREY = 0.62  # linear vertex colours: floor, walls (times the material's base colour)
WALL_WHITE = 1.0
BASE_COLOR = 0.82  # linear albedo of the paint at its whitest
ROUGHNESS = 0.55  # satin studio paint: a faint soft reflection of the car on the floor

ARC_STEPS = 24  # per plan corner
FILLET_STEPS = 16
STEP = 1.0  # metres between rows on the straight walls and up the wall


def lerp(a, b, t):
    return a + (b - a) * t


def plan_path():
    """Points along the walls' line at the floor, left front round the back to right front: (x, z,
    inward normal (nx, nz), distance along)."""
    points = []

    def straight(a, b, normal):
        length = math.hypot(b[0] - a[0], b[1] - a[1])
        count = max(1, round(length / STEP))
        first = 0 if points else -1  # the first straight includes its start; the others continue a run
        for i in range(first, count):
            t = (i + 1) / count
            points.append((lerp(a[0], b[0], t), lerp(a[1], b[1], t), normal))

    def corner(centre, start_angle):
        for i in range(1, ARC_STEPS + 1):
            angle = start_angle + (math.pi / 2) * i / ARC_STEPS
            outward = (math.cos(angle), math.sin(angle))
            points.append((centre[0] + PLAN_RADIUS * outward[0], centre[1] + PLAN_RADIUS * outward[1], (-outward[0], -outward[1])))

    x0, x1 = -HALF_WIDTH, HALF_WIDTH
    z_corner = BACK_Z + PLAN_RADIUS
    # (x, z) plane with angles measured from +x toward +z: the left-back corner's arc runs from
    # pointing -x (pi) to pointing -z (3pi/2).
    straight((x0, FRONT_Z), (x0, z_corner), (1.0, 0.0))
    corner((x0 + PLAN_RADIUS, z_corner), math.pi)
    straight((x0 + PLAN_RADIUS, BACK_Z), (x1 - PLAN_RADIUS, BACK_Z), (0.0, 1.0))
    corner((x1 - PLAN_RADIUS, z_corner), 1.5 * math.pi)
    straight((x1, z_corner), (x1, FRONT_Z), (-1.0, 0.0))

    path, along = [], 0.0
    for i, (x, z, normal) in enumerate(points):
        if i:
            along += math.hypot(x - points[i - 1][0], z - points[i - 1][1])
        path.append((x, z, normal, along))
    return path


def profile():
    """The wall's section from the floor up: (inward distance d, height y, inward normal weight,
    up normal weight, wall share 0 at the floor to 1 up the wall, distance along)."""
    rows = []
    for i in range(FILLET_STEPS + 1):
        phi = (math.pi / 2) * i / FILLET_STEPS
        rows.append((COVE_RADIUS - COVE_RADIUS * math.sin(phi), COVE_RADIUS - COVE_RADIUS * math.cos(phi), math.sin(phi), math.cos(phi), i / FILLET_STEPS))
    count = max(1, round((HEIGHT - COVE_RADIUS) / STEP))
    for i in range(1, count + 1):
        rows.append((0.0, lerp(COVE_RADIUS, HEIGHT, i / count), 1.0, 0.0, 1.0))
    along, out = 0.0, []
    for i, row in enumerate(rows):
        if i:
            along += math.hypot(row[0] - rows[i - 1][0], row[1] - rows[i - 1][1])
        out.append(row + (along,))
    return out


def shade(share):
    """Floor grey to wall white, eased so the change sits in the middle of the fillet."""
    t = share * share * (3.0 - 2.0 * share)
    return lerp(FLOOR_GREY, WALL_WHITE, t)


class Mesh:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.colors, self.indices = [], [], [], [], []

    def vertex(self, position, normal, uv, grey):
        self.positions.append(position)
        self.normals.append(normal)
        self.uvs.append(uv)
        self.colors.append((grey, grey, grey, 1.0))
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        """Wound counter-clockwise seen from the side its vertex normals face."""
        pa, pb, pc = self.positions[a], self.positions[b], self.positions[c]
        e1 = [pb[k] - pa[k] for k in range(3)]
        e2 = [pc[k] - pa[k] for k in range(3)]
        face = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        normal = [self.normals[a][k] + self.normals[b][k] + self.normals[c][k] for k in range(3)]
        if sum(face[k] * normal[k] for k in range(3)) < 0.0:
            b, c = c, b
        self.indices += [a, b, c]


def build():
    mesh = Mesh()
    path = plan_path()
    section = profile()

    # The fillet and the walls: a grid over (path, section).
    grid = []
    for x, z, (nx, nz), along in path:
        column = []
        for d, y, inward, up, share, height_along in section:
            normal = (nx * inward, up, nz * inward)
            column.append(mesh.vertex((x + nx * d, y, z + nz * d), normal, (along / 4.0, height_along / 4.0), shade(share)))
        grid.append(column)
    for i in range(len(grid) - 1):
        for j in range(len(section) - 1):
            a, b, c, d = grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]
            mesh.triangle(a, b, c)
            mesh.triangle(a, c, d)

    # The floor inside the fillet's foot: convex, so a fan from its middle, edged by the fillet's first
    # row (same positions) and closed along the front.
    rim = [mesh.positions[column[0]] for column in grid]
    centre = (0.0, 0.0, (BACK_Z + COVE_RADIUS + FRONT_Z) / 2.0)
    up = (0.0, 1.0, 0.0)
    middle = mesh.vertex(centre, up, (centre[0] / 4.0, centre[2] / 4.0), FLOOR_GREY)
    ring = [mesh.vertex(p, up, (p[0] / 4.0, p[2] / 4.0), FLOOR_GREY) for p in rim]
    # The front edge, from the right front back to the left front, in metre steps.
    front = []
    count = max(1, round((rim[-1][0] - rim[0][0]) / STEP))
    for i in range(1, count):
        x = lerp(rim[-1][0], rim[0][0], i / count)
        front.append(mesh.vertex((x, 0.0, FRONT_Z), up, (x / 4.0, FRONT_Z / 4.0), FLOOR_GREY))
    loop = ring + front
    for i in range(len(loop)):
        mesh.triangle(middle, loop[i], loop[(i + 1) % len(loop)])
    return mesh


def write_gltf(mesh):
    buffer = bytearray()
    views, accessors = [], []

    def add(data, target, component, count, kind, bounds=None):
        while len(buffer) % 4:
            buffer.append(0)
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "target": target})
        buffer.extend(data)
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        if bounds:
            accessor["min"], accessor["max"] = bounds
        accessors.append(accessor)
        return len(accessors) - 1

    count = len(mesh.positions)
    lo = [min(p[k] for p in mesh.positions) for k in range(3)]
    hi = [max(p[k] for p in mesh.positions) for k in range(3)]
    attributes = {
        "POSITION": add(b"".join(struct.pack("<3f", *p) for p in mesh.positions), 34962, 5126, count, "VEC3", (lo, hi)),
        "NORMAL": add(b"".join(struct.pack("<3f", *n) for n in mesh.normals), 34962, 5126, count, "VEC3"),
        "TEXCOORD_0": add(b"".join(struct.pack("<2f", *u) for u in mesh.uvs), 34962, 5126, count, "VEC2"),
        "COLOR_0": add(b"".join(struct.pack("<4f", *c) for c in mesh.colors), 34962, 5126, count, "VEC4"),
    }
    indices = add(b"".join(struct.pack("<I", i) for i in mesh.indices), 34963, 5125, len(mesh.indices), "SCALAR")
    material = {
        "name": "Cyclorama paint",
        "pbrMetallicRoughness": {"baseColorFactor": [BASE_COLOR, BASE_COLOR, BASE_COLOR, 1.0], "metallicFactor": 0.0, "roughnessFactor": ROUGHNESS},
    }
    gltf = {
        "asset": {"version": "2.0", "generator": "tools/render_scenes/make_vehicle_studio.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Cyclorama"}],
        "meshes": [{"name": "Cyclorama", "primitives": [{"attributes": attributes, "indices": indices, "material": 0}]}],
        "materials": [material],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, NAME + ".gltf")
    with open(path, "w", newline="\n") as file:
        json.dump(gltf, file)
    with open(path + ".miniengine_asset.yaml", "w", newline="\n") as file:
        file.write("asset:\n  uuid: %s\n  file: %s" % (MODEL_UUID, NAME + ".gltf"))


def aim(position, target):
    """Euler degrees (XYZ, R = Rx Ry Rz, as BuildLightRotation) that turn an area light's local -Z
    toward target with its width (+X) level."""
    d = [target[k] - position[k] for k in range(3)]
    length = math.sqrt(sum(v * v for v in d))
    d = [v / length for v in d]
    right = [-d[2], 0.0, d[0]]  # d x up, level
    length = math.hypot(right[0], right[2])
    right = [right[0] / length, 0.0, right[2] / length] if length > 1e-6 else [1.0, 0.0, 0.0]
    back = [-v for v in d]  # local +Z
    up = [back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0]]
    # Rows of M = [right | up | back] as columns.
    m = [[right[r], up[r], back[r]] for r in range(3)]
    b = math.asin(max(-1.0, min(1.0, m[0][2])))
    a = math.atan2(-m[1][2], m[2][2])
    c = math.atan2(-m[0][1], m[0][0])
    return [round(math.degrees(v), 4) + 0.0 for v in (a, b, c)]


# (tag, position, aimed at, width x height m, lumens). Lumens over pi x area is the box's luminance.
CAR_CENTRE = (0.0, 0.7, 0.0)
LIGHTS = [
    ("Overhead box", (0.0, 5.2, 0.0), (0.0, 0.0, 0.0), (4.0, 7.0), 220000.0),
    ("Key box", (6.0, 3.0, 5.5), CAR_CENTRE, (3.0, 2.0), 130000.0),
    ("Fill box", (-7.0, 2.6, 4.0), CAR_CENTRE, (3.0, 2.0), 40000.0),
    ("Backdrop light", (0.0, HEIGHT + 0.5, BACK_Z + 2.5), (0.0, 0.0, BACK_Z + 2.5), (20.0, 5.0), 1500000.0),
]
AMBIENT_NITS = 60.0
# The auto exposure meters the white cove to a middle grey (EV100 11.6 from the view in the scene's
# header); these stops bring it to about 8.3, where the backdrop just whites out as in a press photo.
EXPOSURE_COMPENSATION_EV = 3.3


def scene_yaml():
    lines = [
        "# A white photo studio for showing a car and judging rendering: an infinity cove (white walls curving",
        "# into a light grey floor, no edges), soft box area lights and the Skyline R34 in the middle, like a",
        "# maker's press photo. Overhead box (bonnet and roof highlight), key box front-right, fill front-left,",
        "# a panel above the back wall washing the backdrop white; the open top and front see a dim grey Ambient",
        "# light. The white walls' bounce comes from the DDGI probes (and ray traced reflections or path tracing,",
        "# where on). exposure_compensation_ev keeps the auto exposure from metering the white studio to grey.",
        "# Written by tools/render_scenes/make_vehicle_studio.py, which says how to change the cove and lights.",
        "# A good view: camera 4.2, 1.0, 4.6, yaw -132, pitch -3, fov 40 (front three quarters, the car's nose",
        "# to the left). Needs the cove at assets/models/studio_cove (tests/fixtures/render_scenes/models/studio_cove,",
        "# from scripts/install-render-scenes.*) and the car imported from Assetto Corsa at",
        "# assets/skyline_r34_vspec (not in git: the game's content), run from the repository's root.",
        "scene:",
        "  version: 3",
        "  selected_entity_uuid: 57d10c0e-1a2b-4c3d-8e4f-000000000102",
        "  selected_entity: 1",
        "entities:",
        "  - entity_uuid: 57d10c0e-1a2b-4c3d-8e4f-000000000101",
        "    tag: Studio cove",
        "    model:",
        "      display_name: studio_cove.gltf",
        "      source_path: assets/models/studio_cove/studio_cove.gltf",
        "      source_uuid: " + MODEL_UUID,
        '      base_color_texture_override: ""',
        '      base_color_texture_override_uuid: ""',
        "    transform:",
        "      translation: [0, 0, 0]",
        "      rotation: [0, 0, 0]",
        "      scale: [1, 1, 1]",
        "  - entity_uuid: 57d10c0e-1a2b-4c3d-8e4f-000000000102",
        "    tag: skyline_r34_vspec",
        "    model:",
        "      display_name: skyline_r34_vspec.gltf",
        "      source_path: assets/skyline_r34_vspec/skyline_r34_vspec.gltf",
        "      source_uuid: 35d02ab2-1e85-4424-bcb6-174ffc06be03",
        '      base_color_texture_override: ""',
        '      base_color_texture_override_uuid: ""',
        "    transform:",
        "      translation: [0, 0, 0]",
        "      rotation: [0, 180, 0]",
        "      scale: [1, 1, 1]",
        "lights:",
    ]

    def light(index, tag, kind, color, intensity, size, translation, rotation, shadows=True):
        lines.extend([
            "  - entity_uuid: 57d10c0e-1a2b-4c3d-8e4f-%012d" % (200 + index),
            "    tag: " + tag,
            "    light_type: " + kind,
            "    color: [%g, %g, %g]" % color,
            "    intensity: %g" % intensity,
            "    range: 40",
            "    spot_inner_angle: 20",
            "    spot_outer_angle: 35",
            "    area_size: [%g, %g]" % size,
            "    cast_shadows: " + ("true" if shadows else "false"),
            "    source_radius: 0",
            "    ground_color: [0.3, 0.25, 0.2]",
            "    transform:",
            "      translation: [%g, %g, %g]" % translation,
            "      rotation: [%g, %g, %g]" % tuple(rotation),
            "      scale: [1, 1, 1]",
        ])

    for index, (tag, position, target, size, lumens) in enumerate(LIGHTS):
        light(index, tag, "area", (1.0, 1.0, 1.0), lumens, size, position, aim(position, target))
    light(len(LIGHTS), "Studio ambient", "ambient", (1.0, 1.0, 1.0), AMBIENT_NITS, (1.0, 1.0), (0.0, 4.0, 0.0), [0, 0, 0], shadows=False)
    lines.extend([
        "environment:",
        "  mode: none",
        "  exposure_compensation_ev: %g" % EXPOSURE_COMPENSATION_EV,
        "editor:",
        "  gizmo:",
        "    operation: combined",
        "    mode: world",
        "    use_snap: false",
        "    translation_snap: [1, 1, 1]",
        "    rotation_snap: 15",
        "    scale_snap: [1, 1, 1]",
    ])
    with open(SCENE, "w", newline="\n") as file:
        file.write("\n".join(lines) + "\n")


def main():
    mesh = build()
    write_gltf(mesh)
    scene_yaml()
    print("%d vertices, %d triangles -> %s, %s" % (len(mesh.positions), len(mesh.indices) // 3, os.path.normpath(OUT_DIR), os.path.normpath(SCENE)))


if __name__ == "__main__":
    main()
