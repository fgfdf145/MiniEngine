#!/usr/bin/env python3
"""Writes the vehicle test track (tests/fixtures/render_scenes/models/car_test_track/), its test car
(.../models/car_test_car/) and a plain asphalt ground (.../models/asphalt_ground/).

Three courses leave one asphalt apron. The car stands on the apron facing +Z, in front of the first.

  Suspension (x -6..6, 170 m): speed humps, a washboard, bumps under one side only and then the other,
      a kerb up and down, and a jump ramp. Yellow where the ground is raised.
  Body attitude (x -40..-28, 275 m): climbs of 6 and 12 degrees and the descents back, three
      sine waves for heave and pitch, and a banked road whose bank swings 0, +12, -12, 0 degrees for roll.
  Grip (x 28..60): eight 4 m lanes side by side over 100 m, from dry asphalt to ice; then an 8 m road
      whose left half is asphalt and right half ice (split friction), then bands of asphalt, ice and gravel.
      Each surface grips as Wong's table of road adhesion has it (see SURFACES and GRIP).

The track collides through its own MINIENGINE_collision nodes, each surface at its friction (the same
meshes are also drawn). Posts every 10 m beside the courses are only drawn. Metres, Y up, ground at y = 0.

    python3 tools/render_scenes/make_car_test_track.py
"""

import base64
import json
import math
import os
import struct

MODELS_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "fixtures", "render_scenes", "models")

# Surface name: (colour, roughness, friction or None when the surface is only drawn). The friction is a
# ratio to dry asphalt that multiplies the tyre's own coefficient (as Assetto Corsa's surfaces.ini does).
# Peak / sliding coefficients from J.Y. Wong, Theory of Ground Vehicles, Table 1.3 ("average values of
# coefficient of road adhesion"), over dry asphalt's 0.85 (0.8-0.9): wet asphalt 0.5-0.7 / 0.45-0.6,
# gravel 0.6 / 0.55, dry earth road 0.68 / 0.65, wet earth road 0.55 / 0.4-0.5, hard-packed snow
# 0.2 / 0.15, ice 0.1 / 0.07; concrete is dry asphalt's (Burckhardt: 1.09 against 1.17). Dry mown grass
# is not in the table: 0.4-0.5 in the literature.
SURFACES = {
    "asphalt": ((0.045, 0.045, 0.05, 1.0), 0.85, 1.0),
    "hazard": ((0.70, 0.50, 0.03, 1.0), 0.6, 1.0),  # raised asphalt, yellow to see it
    "concrete": ((0.35, 0.35, 0.34, 1.0), 0.9, 0.95),
    "wet_asphalt": ((0.02, 0.022, 0.028, 1.0), 0.12, 0.95),  # at a standstill; falls with speed, see GRIP
    "grass": ((0.06, 0.16, 0.03, 1.0), 1.0, 0.53),
    "dirt": ((0.16, 0.09, 0.045, 1.0), 1.0, 0.8),
    "gravel": ((0.28, 0.26, 0.23, 1.0), 1.0, 0.71),
    "mud": ((0.07, 0.045, 0.03, 1.0), 0.5, 0.65),  # wet earth
    "snow": ((0.75, 0.78, 0.80, 1.0), 0.7, 0.24),
    "ice": ((0.55, 0.70, 0.80, 1.0), 0.05, 0.12),
    "marking": ((0.70, 0.70, 0.70, 1.0), 0.7, None),
    "post_white": ((0.75, 0.75, 0.75, 1.0), 0.6, None),
    "post_red": ((0.7, 0.03, 0.03, 1.0), 0.6, None),
}


# How the rest of each surface grips (MINIENGINE_collision, PhysicsWorld's SurfaceGrip). Off the pavement
# the ground, not the rubber, sets the limit: frictionCap is the absolute coefficient (Wong's peak), which
# a race slick does not pass. slidingShare is Wong's sliding over peak. Wet asphalt's grip falls with
# speed, exp(-wetSpeedFalloff * v): 0.7 of dry at 30 km/h to 0.5 at 100 km/h, the ends of Wong's range.
# rollingResistance is added to the tyre's own 0.012, after the Bosch Automotive Handbook's car-tyre
# coefficients: rolled gravel 0.02, unpaved road 0.05, field / grass 0.1 and up; packed snow about 0.025.
GRIP = {
    "wet_asphalt": {"wetSpeedFalloff": 0.0173, "slidingShare": 0.86},
    "grass": {"frictionCap": 0.45, "slidingShare": 0.9, "rollingResistance": 0.06},
    "dirt": {"frictionCap": 0.68, "slidingShare": 0.96, "rollingResistance": 0.037},
    "gravel": {"frictionCap": 0.6, "slidingShare": 0.92, "rollingResistance": 0.012},
    "mud": {"frictionCap": 0.55, "slidingShare": 0.82, "rollingResistance": 0.09},
    "snow": {"frictionCap": 0.2, "slidingShare": 0.75, "rollingResistance": 0.013},
    "ice": {"frictionCap": 0.1, "slidingShare": 0.7},
}


class Mesh:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []

    def vertex(self, position, normal):
        self.positions.append(position)
        self.normals.append(normal)
        self.uvs.append((position[0] * 0.25, position[2] * 0.25))
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        """Three vertex indices; the winding is flipped when it faces against the vertices' normals."""
        pa, pb, pc = (self.positions[i] for i in (a, b, c))
        u = [pb[k] - pa[k] for k in range(3)]
        v = [pc[k] - pa[k] for k in range(3)]
        face = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        if face == (0, 0, 0) or all(abs(f) < 1e-12 for f in face):
            return
        na = self.normals[a]
        if sum(face[k] * na[k] for k in range(3)) < 0.0:
            b, c = c, b
        self.indices += [a, b, c]

    def quad(self, a, b, c, d, normal):
        ids = [self.vertex(p, normal) for p in (a, b, c, d)]
        self.triangle(ids[0], ids[1], ids[2])
        self.triangle(ids[0], ids[2], ids[3])

    def box(self, lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        self.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), (0, 1, 0))
        self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0))
        self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1))
        self.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (0, 0, -1))
        self.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (1, 0, 0))
        self.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (-1, 0, 0))

    def cylinder_x(self, center, radius, half_width, segments=28):
        """A cylinder whose axis is X."""
        cx, cy, cz = center
        for i in range(segments):
            a0 = 2.0 * math.pi * i / segments
            a1 = 2.0 * math.pi * (i + 1) / segments
            n0 = (0.0, math.cos(a0), math.sin(a0))
            n1 = (0.0, math.cos(a1), math.sin(a1))
            ids = [
                self.vertex((cx - half_width, cy + radius * n0[1], cz + radius * n0[2]), n0),
                self.vertex((cx + half_width, cy + radius * n0[1], cz + radius * n0[2]), n0),
                self.vertex((cx + half_width, cy + radius * n1[1], cz + radius * n1[2]), n1),
                self.vertex((cx - half_width, cy + radius * n1[1], cz + radius * n1[2]), n1),
            ]
            self.triangle(ids[0], ids[1], ids[2])
            self.triangle(ids[0], ids[2], ids[3])
            for side in (-1.0, 1.0):
                x = cx + side * half_width
                centre = self.vertex((x, cy, cz), (side, 0.0, 0.0))
                p0 = self.vertex((x, cy + radius * n0[1], cz + radius * n0[2]), (side, 0.0, 0.0))
                p1 = self.vertex((x, cy + radius * n1[1], cz + radius * n1[2]), (side, 0.0, 0.0))
                self.triangle(centre, p0, p1)

    def flat_rect(self, x0, x1, z0, z1, y=0.0):
        self.quad((x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0), (0, 1, 0))


def axis_samples(segments):
    """Sorted sample positions from (start, end, step) segments; every segment ends exactly at its end."""
    values = []
    for start, end, step in segments:
        count = max(1, round((end - start) / step))
        values += [start + (end - start) * i / count for i in range(count)]
    values.append(segments[-1][1])
    return values


def heightfield(meshes, xs, zs, height, surface_of):
    """A ground surface over the grid xs x zs (X, Z ascending): height(x, z) in metres, and the surface of
    each cell from surface_of(max corner height, x centre, z centre). Normals from the height's slope."""
    step = 1e-3

    def normal(x, z):
        dx = (height(x + step, z) - height(x - step, z)) / (2.0 * step)
        dz = (height(x, z + step) - height(x, z - step)) / (2.0 * step)
        length = math.sqrt(dx * dx + 1.0 + dz * dz)
        return (-dx / length, 1.0 / length, -dz / length)

    for i in range(len(xs) - 1):
        for j in range(len(zs) - 1):
            x0, x1, z0, z1 = xs[i], xs[i + 1], zs[j], zs[j + 1]
            corners = [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]
            heights = [height(x, z) for x, z in corners]
            surface = surface_of(max(heights), (x0 + x1) * 0.5, (z0 + z1) * 0.5)
            mesh = meshes[surface]
            ids = [mesh.vertex((x, h, z), normal(x, z)) for (x, z), h in zip(corners, heights)]
            mesh.triangle(ids[0], ids[1], ids[2])
            mesh.triangle(ids[0], ids[2], ids[3])


def skirt(mesh, xs_edge, zs, height, bottom):
    """A wall under a heightfield's edge at x = xs_edge, down to y = bottom, facing outward (+X or -X
    by which side of the centre line the edge is on)."""
    outward = 1.0 if xs_edge > 0 else -1.0
    for j in range(len(zs) - 1):
        z0, z1 = zs[j], zs[j + 1]
        mesh.quad(
            (xs_edge, bottom, z0), (xs_edge, bottom, z1), (xs_edge, height(xs_edge, z1), z1), (xs_edge, height(xs_edge, z0), z0), (outward, 0, 0)
        )


def hump(z, center, length, rise):
    """A raised cosine bump of the given length and rise centred on `center`."""
    half = length * 0.5
    if abs(z - center) >= half:
        return 0.0
    return rise * 0.5 * (1.0 + math.cos(math.pi * (z - center) / half))


def smoothstep(value):
    value = min(max(value, 0.0), 1.0)
    return value * value * (3.0 - 2.0 * value)


def suspension_course(meshes):
    """x -6..6, z 0..170."""
    # (centre, length, rise) of the humps across the whole road, then of those under one side only.
    humps = [(15.0, 1.5, 0.06), (27.0, 2.0, 0.10), (40.0, 3.0, 0.15)]
    left_bumps = [(70.0, 1.5, 0.12), (73.0, 1.5, 0.12), (76.0, 1.5, 0.12), (94.0, 1.5, 0.12), (100.0, 1.5, 0.12)]
    right_bumps = [(82.0, 1.5, 0.12), (85.0, 1.5, 0.12), (88.0, 1.5, 0.12), (97.0, 1.5, 0.12), (103.0, 1.5, 0.12)]

    def height(x, z):
        h = sum(hump(z, c, l, r) for c, l, r in humps)
        if 52.0 <= z <= 61.6:  # washboard: 16 ridges, 0.6 m period, 0.05 m high
            h += 0.05 * 0.5 * (1.0 - math.cos(2.0 * math.pi * (z - 52.0) / 0.6))
        # The side a bump sits on changes within 0.1 m at the centre line.
        left_weight = smoothstep((0.05 - x) / 0.1)
        right_weight = smoothstep((x + 0.05) / 0.1)
        h += left_weight * sum(hump(z, c, l, r) for c, l, r in left_bumps)
        h += right_weight * sum(hump(z, c, l, r) for c, l, r in right_bumps)
        if 112.0 <= z <= 122.0:  # kerb up, plateau, kerb down
            h += 0.12 * smoothstep((z - 112.0) / 0.2) * smoothstep((122.2 - z) / 0.2)
        if 130.0 <= z <= 138.4:  # jump ramp: 8 degrees up over 7 m, then a drop
            h += min((z - 130.0) * math.tan(math.radians(8.0)), 1.0) if z < 137.0 else 0.0
        return h

    xs = [-6.0, -0.06, 0.06, 6.0]
    zs = axis_samples(
        [(0.0, 10.0, 5.0), (10.0, 45.0, 0.25), (45.0, 52.0, 3.5), (52.0, 62.0, 0.1), (62.0, 66.0, 2.0)]
        + [(66.0, 110.0, 0.25), (110.0, 124.0, 0.1), (124.0, 129.0, 1.0), (129.0, 138.0, 0.1), (138.0, 170.0, 8.0)]
    )
    heightfield(meshes, xs, zs, height, lambda h, x, z: "hazard" if h > 0.004 else "asphalt")


def attitude_course(meshes):
    """x -40..-28, z 0..275."""
    center = -34.0
    # Slope (length, degrees) pieces: 6 and 12 degrees up, then down again, 3 m of blending between.
    pieces = [(10, 0), (20, 6), (10, 0), (15, 12), (10, 0), (15, -12), (10, 0), (20, -6), (10, 0)]
    step = 0.1
    bounds = []
    z = 0.0
    for length, degrees in pieces:
        bounds.append((z, z + length, math.tan(math.radians(degrees))))
        z += length
    slope_end = z

    def raw_slope(z):
        for start, end, slope in bounds:
            if start <= z < end:
                return slope
        return 0.0

    def smooth_slope(z):
        return sum(raw_slope(z + offset * 0.1) for offset in range(-15, 16)) / 31.0

    profile = [0.0]
    for i in range(int(slope_end / step)):
        profile.append(profile[-1] + smooth_slope((i + 0.5) * step) * step)

    wave_start, wave_period, wave_count, wave_amplitude = 125.0, 10.0, 3, 0.35
    bank_pieces = [(165.0, 185.0, 0.0, 12.0), (185.0, 205.0, 12.0, 12.0), (205.0, 235.0, 12.0, -12.0), (235.0, 255.0, -12.0, -12.0), (255.0, 275.0, -12.0, 0.0)]

    def bank_degrees(z):
        for start, end, a, b in bank_pieces:
            if start <= z <= end:
                return a + (b - a) * smoothstep((z - start) / (end - start))
        return 0.0

    def height(x, z):
        if z < slope_end:
            index = min(max(z / step, 0.0), len(profile) - 1.001)
            i = int(index)
            return profile[i] + (profile[i + 1] - profile[i]) * (index - i)
        if wave_start <= z <= wave_start + wave_period * wave_count:
            return wave_amplitude * 0.5 * (1.0 - math.cos(2.0 * math.pi * (z - wave_start) / wave_period))
        return (x - center) * math.tan(math.radians(bank_degrees(z)))

    xs = [center - 6.0, center + 6.0]
    zs = axis_samples([(0.0, 120.0, 0.5), (120.0, 125.0, 5.0), (125.0, 155.0, 0.5), (155.0, 165.0, 5.0), (165.0, 275.0, 0.5)])
    # The banked stretch is one cell wide across; split it so the cross slope is a plane per cell strip.
    bank_xs = [center - 6.0 + 2.0 * i for i in range(7)]
    heightfield(meshes, xs, [z for z in zs if z <= 165.0], height, lambda h, x, z: "concrete")
    heightfield(meshes, bank_xs, [z for z in zs if z >= 165.0], height, lambda h, x, z: "concrete")
    bank_zs = [z for z in zs if z >= 165.0]
    skirt(meshes["concrete"], center + 6.0, bank_zs, height, -0.3)
    skirt(meshes["concrete"], center - 6.0, bank_zs, height, -0.3)


def grip_lanes(meshes):
    """Eight 4 m lanes over x 28..60, z 0..100, then the split-friction road and friction bands."""
    lanes = ["asphalt", "wet_asphalt", "grass", "dirt", "gravel", "mud", "snow", "ice"]
    for index, surface in enumerate(lanes):
        meshes[surface].flat_rect(28.0 + 4.0 * index, 32.0 + 4.0 * index, 0.0, 100.0)
        meshes["marking"].flat_rect(28.0 + 4.0 * index - 0.06, 28.0 + 4.0 * index + 0.06, 0.0, 100.0, 0.004)
    meshes["marking"].flat_rect(60.0 - 0.06, 60.0 + 0.06, 0.0, 100.0, 0.004)
    # Split friction: asphalt under the left wheels, ice under the right.
    meshes["asphalt"].flat_rect(36.0, 44.0, 110.0, 150.0)
    meshes["ice"].flat_rect(44.0, 52.0, 110.0, 150.0)
    meshes["marking"].flat_rect(44.0 - 0.06, 44.0 + 0.06, 110.0, 150.0, 0.004)
    # Friction changes along the road.
    for surface, z0, z1 in (("asphalt", 155.0, 165.0), ("ice", 165.0, 175.0), ("gravel", 175.0, 185.0)):
        meshes[surface].flat_rect(36.0, 52.0, z0, z1)


# Flat areas (x0, x1, z0, z1) the grass must leave out; the course and lane rectangles included.
COVERED = [
    (-60.0, 70.0, -60.0, 0.0),
    (-6.0, 6.0, 0.0, 170.0),
    (-40.0, -28.0, 0.0, 275.0),
    (28.0, 60.0, 0.0, 100.0),
    (36.0, 52.0, 110.0, 150.0),
    (36.0, 52.0, 155.0, 185.0),
]


def grass_around(meshes):
    xs = sorted({-200.0, 200.0} | {v for r in COVERED for v in r[:2]})
    zs = sorted({-200.0, 400.0} | {v for r in COVERED for v in r[2:]})
    for i in range(len(xs) - 1):
        for j in range(len(zs) - 1):
            cx, cz = (xs[i] + xs[i + 1]) * 0.5, (zs[j] + zs[j + 1]) * 0.5
            if not any(r[0] < cx < r[1] and r[2] < cz < r[3] for r in COVERED):
                meshes["grass"].flat_rect(xs[i], xs[i + 1], zs[j], zs[j + 1])


def apron_markings(meshes):
    """The apron's lane lines in front of each course, and the car's start line."""
    marking = meshes["marking"]
    marking.flat_rect(-60.0, 70.0, -0.15, 0.15, 0.004)
    for x in (0.0, -34.0, 44.0):
        marking.flat_rect(x - 0.06, x + 0.06, -14.0, 0.0, 0.004)


def posts(meshes):
    """Posts every 10 m beside each course, red every 50 m; only drawn."""
    for x in (-7.5, 7.5, -41.5, -26.5, 26.5, 61.5):
        length = 275.0 if -45.0 < x < -20.0 else 170.0 if abs(x) < 10.0 else 100.0
        z = 0.0
        while z <= length:
            meshes["post_red" if z % 50.0 == 0.0 else "post_white"].box((x - 0.08, 0.0, z - 0.08), (x + 0.08, 1.2, z + 0.08))
            z += 10.0


def build_track():
    meshes = {name: Mesh() for name in SURFACES}
    meshes["asphalt"].flat_rect(-60.0, 70.0, -60.0, 0.0)
    suspension_course(meshes)
    attitude_course(meshes)
    grip_lanes(meshes)
    grass_around(meshes)
    apron_markings(meshes)
    posts(meshes)
    return {name: mesh for name, mesh in meshes.items() if mesh.indices}


def build_asphalt_ground():
    """A plain 200 m square of the track's dry asphalt, centred on the origin at y = 0, drawn and colliding:
    ground for scenes that need nothing else (the suspension rig's)."""
    ground = Mesh()
    ground.flat_rect(-100.0, 100.0, -100.0, 100.0)
    return {"asphalt": ground}


def build_car():
    meshes = {name: Mesh() for name in ("body", "cabin", "wheel", "light_front", "light_rear")}
    meshes["body"].box((-0.8, 0.28, -2.2), (0.8, 0.85, 2.2))
    meshes["cabin"].box((-0.74, 0.85, -1.1), (0.74, 1.4, 1.0))
    front_z, rear_z = 1.408, -1.32
    for x in (-0.79, 0.79):
        for z in (front_z, rear_z):
            meshes["wheel"].cylinder_x((x, 0.32, z), 0.32, 0.108)
    for x in (-0.55, 0.35):
        meshes["light_front"].box((x, 0.5, 2.2), (x + 0.2, 0.62, 2.22))
        meshes["light_rear"].box((x, 0.5, -2.22), (x + 0.2, 0.62, -2.2))
    return meshes


CAR_MATERIALS = {
    "body": ((0.6, 0.02, 0.02, 1.0), 0.3, 0.5),
    "cabin": ((0.02, 0.03, 0.04, 1.0), 0.1, 0.0),
    "wheel": ((0.02, 0.02, 0.02, 1.0), 0.9, 0.0),
    "light_front": ((0.9, 0.9, 0.8, 1.0), 0.2, 0.0),
    "light_rear": ((0.5, 0.0, 0.0, 1.0), 0.3, 0.0),
}


def write_gltf(path, meshes, materials, collision_friction, generator):
    """One mesh and material per entry of meshes. A drawn node each, and for every surface with a
    friction a second node of the same mesh carrying MINIENGINE_collision."""
    buffer = bytearray()
    views = []
    accessors = []

    def add_view(data, target):
        while len(buffer) % 4:
            buffer.append(0)
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "target": target})
        buffer.extend(data)
        return len(views) - 1

    gltf_meshes, gltf_materials, nodes = [], [], []
    for name, mesh in meshes.items():
        count = len(mesh.positions)
        lo = [min(p[k] for p in mesh.positions) for k in range(3)]
        hi = [max(p[k] for p in mesh.positions) for k in range(3)]
        base = len(accessors)
        accessors.append({"bufferView": add_view(b"".join(struct.pack("<3f", *p) for p in mesh.positions), 34962), "componentType": 5126, "count": count, "type": "VEC3", "min": lo, "max": hi})
        accessors.append({"bufferView": add_view(b"".join(struct.pack("<3f", *n) for n in mesh.normals), 34962), "componentType": 5126, "count": count, "type": "VEC3"})
        accessors.append({"bufferView": add_view(b"".join(struct.pack("<2f", *u) for u in mesh.uvs), 34962), "componentType": 5126, "count": count, "type": "VEC2"})
        accessors.append({"bufferView": add_view(b"".join(struct.pack("<I", i) for i in mesh.indices), 34963), "componentType": 5125, "count": len(mesh.indices), "type": "SCALAR"})
        color, roughness, extra = materials[name]
        gltf_materials.append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(color), "metallicFactor": extra if collision_friction is None else 0.0, "roughnessFactor": roughness}})
        gltf_meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2}, "indices": base + 3, "material": len(gltf_materials) - 1}]})
        nodes.append({"mesh": len(gltf_meshes) - 1, "name": name})
        friction = None if collision_friction is None else collision_friction(name)
        if friction is not None:
            collision = {"surface": name, "friction": friction}
            collision.update(GRIP.get(name, {}))
            nodes.append({"mesh": len(gltf_meshes) - 1, "name": name + "_collision", "extensions": {"MINIENGINE_collision": collision}})

    gltf = {
        "asset": {"version": "2.0", "generator": generator},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": gltf_meshes,
        "materials": gltf_materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    if collision_friction is not None:
        gltf["extensionsUsed"] = ["MINIENGINE_collision"]
    with open(path, "w") as file:
        json.dump(gltf, file)


def write_model(name, meshes, materials, collision_friction, uuid):
    out_dir = os.path.join(MODELS_DIR, name)
    os.makedirs(out_dir, exist_ok=True)
    write_gltf(os.path.join(out_dir, name + ".gltf"), meshes, materials, collision_friction, "tools/render_scenes/make_car_test_track.py")
    sidecar = os.path.join(out_dir, name + ".gltf.miniengine_asset.yaml")
    if not os.path.exists(sidecar):  # keeps the uuid a scene refers to
        with open(sidecar, "w") as file:
            file.write("asset:\n  uuid: {}\n  file: {}.gltf".format(uuid, name))


def main():
    track = build_track()
    write_model("car_test_track", track, {n: (SURFACES[n][0], SURFACES[n][1], 0.0) for n in track}, lambda n: SURFACES[n][2], "7c2e5a90-1d43-4b6f-8a17-3e9d0b5c2f41")
    write_model("car_test_car", build_car(), CAR_MATERIALS, None, "a4f81d36-92b7-4c0e-b5d3-6e1f7a28c904")
    write_model("asphalt_ground", build_asphalt_ground(), {"asphalt": (SURFACES["asphalt"][0], SURFACES["asphalt"][1], 0.0)}, lambda n: SURFACES[n][2], "ac711dfe-e1a7-41db-951e-b95b7f80c4e8")
    for name, mesh in track.items():
        print("{:12s} {:7d} triangles".format(name, len(mesh.indices) // 3))


if __name__ == "__main__":
    main()
