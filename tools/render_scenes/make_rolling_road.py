#!/usr/bin/env python3
"""Writes a gently undulating road for testing a car's suspension (assets/models/rolling_road/) and a
scene that stands the Skyline R34 on it (assets/scenes/rolling_road.yaml).

The road is a height map: rolling_road_height.png, 16 bits, one pixel per 0.25 m, 48 pixels across the
road (x -6..6) by 1200 along it (z 0..300). A pixel's value maps linearly to a height from -HEIGHT_RANGE
to +HEIGHT_RANGE metres (flat at 32768). The map is built from a few sine waves of different wavelengths
(long swells that heave and pitch the body, shorter ones that move the wheels) whose crests tilt across
the road, so the two sides rise and fall at different places and the body rolls too. It fades to flat over
the first and last 20 m. The mesh is read back from the PNG, so editing the picture changes the road.

The road is drawn and collides as asphalt (the model's MINIENGINE_collision node). Metres, Y up.

    python3 tools/render_scenes/make_rolling_road.py
"""

import math
import os
import re
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import make_car_test_track as track  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
MODEL_DIR = os.path.join(ROOT, "assets", "models", "rolling_road")
HEIGHT_MAP = os.path.join(MODEL_DIR, "rolling_road_height.png")
SCENE = os.path.join(ROOT, "assets", "scenes", "rolling_road.yaml")
RIG_SCENE = os.path.join(ROOT, "assets", "scenes", "suspension_rig.yaml")

PIXEL = 0.25  # metres per pixel
HALF_WIDTH = 6.0
LENGTH = 300.0
HEIGHT_RANGE = 0.10  # metres either side of flat that the PNG's full range spans
FADE = 20.0  # metres at each end over which the road comes down to flat
# (wavelength along the road m, amplitude m, crest tilt across the road, radians)
WAVES = [(24.0, 0.030, 0.10), (13.0, 0.022, -0.22), (7.5, 0.016, 0.35), (4.2, 0.008, -0.5)]
ROAD_UUID = "5d1f0a86-3b7c-4e29-9a54-c8e6b2d7f013"
SEED = 7


def make_height_map():
    columns = round(2 * HALF_WIDTH / PIXEL)
    rows = round(LENGTH / PIXEL)
    x = (np.arange(columns) + 0.5) * PIXEL - HALF_WIDTH
    z = (np.arange(rows) + 0.5) * PIXEL
    xx, zz = np.meshgrid(x, z)
    rng = np.random.default_rng(SEED)
    height = np.zeros_like(xx)
    for wavelength, amplitude, tilt in WAVES:
        phase = rng.uniform(0.0, 2.0 * math.pi)
        # Crests run across the road at `tilt` to the perpendicular, so the sides are out of step.
        height += amplitude * np.sin(2.0 * math.pi * (zz + xx * math.tan(tilt)) / wavelength + phase)
    ramp = np.clip(np.minimum(zz, LENGTH - zz) / FADE, 0.0, 1.0)
    height *= ramp * ramp * (3.0 - 2.0 * ramp)
    pixels = np.clip(np.round(height / HEIGHT_RANGE * 32767.0 + 32768.0), 0, 65535).astype(np.uint16)
    os.makedirs(MODEL_DIR, exist_ok=True)
    Image.fromarray(pixels).save(HEIGHT_MAP)


def build_road():
    image = np.asarray(Image.open(HEIGHT_MAP)).astype(np.float64)
    heights = (image - 32768.0) / 32767.0 * HEIGHT_RANGE
    rows, columns = heights.shape

    def height(x, z):
        """Bilinear sample at world (x, z); pixel centres sit at (i + 0.5) * PIXEL."""
        u = min(max((x + HALF_WIDTH) / PIXEL - 0.5, 0.0), columns - 1.0)
        v = min(max(z / PIXEL - 0.5, 0.0), rows - 1.0)
        i, j = min(int(u), columns - 2), min(int(v), rows - 2)
        fu, fv = u - i, v - j
        top = heights[j, i] * (1 - fu) + heights[j, i + 1] * fu
        bottom = heights[j + 1, i] * (1 - fu) + heights[j + 1, i + 1] * fu
        return float(top * (1 - fv) + bottom * fv)

    xs = [-HALF_WIDTH + PIXEL * (k + 0.5) for k in range(columns)]
    xs = [-HALF_WIDTH] + xs + [HALF_WIDTH]
    zs = [0.0] + [PIXEL * (k + 0.5) for k in range(rows)] + [LENGTH]
    meshes = {"asphalt": track.Mesh()}
    track.heightfield(meshes, xs, zs, height, lambda *_: "asphalt")
    track.skirt(meshes["asphalt"], -HALF_WIDTH, zs, height, -0.3)
    track.skirt(meshes["asphalt"], HALF_WIDTH, zs, height, -0.3)
    return meshes


def write_scene():
    rig = open(RIG_SCENE).read()
    tail = rig[rig.index("lights:"):]
    car = re.search(r"  - entity_uuid: fdea4903.*?scale: \[1, 1, 1\]\n", rig, re.S).group(0)
    car = car.replace("translation: [0, 0.6506, 0]", "translation: [0, 0.0006, 5]")
    road = """  - entity_uuid: 9b3e7c14-52a8-4d60-8f1e-0a6d4c2e5b97
    tag: Rolling road
    model:
      display_name: rolling_road.gltf
      source_path: assets/models/rolling_road/rolling_road.gltf
      source_uuid: {uuid}
      base_color_texture_override: ""
      base_color_texture_override_uuid: ""
    transform:
      translation: [0, 0, 0]
      rotation: [0, 0, 0]
      scale: [1, 1, 1]
""".format(uuid=ROAD_UUID)
    header = (
        "# The Nissan Skyline R34 V-Spec on a 300 m road with gentle undulations (a few centimetres: swells\n"
        "# that heave and pitch the body, shorter waves that move the wheels, crests tilted across the road\n"
        "# so it rolls too), for testing the suspension by driving. The car stands at the start, facing +Z.\n"
        "# The road comes from tools/render_scenes/make_rolling_road.py. Needs the car imported from Assetto\n"
        "# Corsa at assets/skyline_r34_vspec (not in git), run from the repository's root.\n"
        "scene:\n  version: 3\n  selected_entity_uuid: fdea4903-ce20-40f3-a012-ac1e44d8b155\n  selected_entity: 1\nentities:\n"
    )
    with open(SCENE, "w", newline="\n") as file:
        file.write(header + road + car + tail)


def main():
    make_height_map()
    meshes = build_road()
    track.write_gltf(os.path.join(MODEL_DIR, "rolling_road.gltf"), meshes, {"asphalt": (track.SURFACES["asphalt"][0], track.SURFACES["asphalt"][1], 0.0)}, lambda n: track.SURFACES[n][2], "tools/render_scenes/make_rolling_road.py")
    sidecar = os.path.join(MODEL_DIR, "rolling_road.gltf.miniengine_asset.yaml")
    if not os.path.exists(sidecar):
        with open(sidecar, "w") as file:
            file.write("asset:\n  uuid: {}\n  file: rolling_road.gltf".format(ROAD_UUID))
    write_scene()
    print("{} triangles".format(len(meshes["asphalt"].indices) // 3))


if __name__ == "__main__":
    main()
