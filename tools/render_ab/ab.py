"""A/B captures for renderer changes that must not change the image
(docs/design/2026-10-08-slang-shader-migration-design.md, 2026-10-08-nvrhi-backend-design.md).

Runs each case three times: A twice (the noise floor) and B once, then compares the PNGs.
  AB_MODE=shaders (default): the same exe with --shaders at a reference SPIR-V folder (A, AB_REF_SPV)
                             and at the build's own shaders (B).
  AB_MODE=exe: A is a baseline build in out/baseline_src (a git worktree of the commit before the
               change, built with the same preset), B the current build.
usage: python tools/render_ab/ab.py [case names...]   (all cases when none given)
env: AB_PRESET (default vs2026-x64; a single-config preset such as linux-debug has no Debug folder),
     AB_BUILD (default out/build/<preset>: the build B runs, its app and its shaders),
     AB_BASELINE_BUILD (default out/baseline_src/out/build/<preset>: AB_MODE=exe's A build),
     AB_EXE (default AB_BUILD's Debug app), AB_FRAMES (default 90), AB_SIZE (default 1280x720),
     AB_ASSETS (default the main checkout's assets), AB_OUT (default out/render_ab),
     AB_REUSE_A (an earlier run's AB_OUT: its A captures stand in for this run's, so only B runs; for
     AB_MODE=exe against the same baseline, AB_SIZE and AB_FRAMES)
The fixture_* cases need only the repository's render scenes (tests/fixtures/render_scenes, installed
into AB_ASSETS by scripts/install-render-scenes.sh), so they run where the R34 and Yuki do not exist,
for example on lavapipe at a small AB_SIZE.
Test scenes (tools/render_ab/scenes) have the atmosphere's ground plane off: on the rolling road it
z-fights the road and the TAA jitter phase moves the patches from run to run.
"""

import json
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.environ.get("AB_OUT") or os.path.join(ROOT, "out", "render_ab")
SCENES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes")
PRESET = os.environ.get("AB_PRESET", "vs2026-x64")
# The Visual Studio generator puts each configuration in a folder of its own; Ninja presets do not.
APP = os.path.join("app", "Debug", "miniengine_app.exe") if PRESET.startswith("vs") else os.path.join(
    "app", "miniengine_app.exe" if os.name == "nt" else "miniengine_app")
BUILD = os.environ.get("AB_BUILD") or os.path.join(ROOT, "out", "build", PRESET)
EXE = os.environ.get("AB_EXE", os.path.join(BUILD, APP))
ASSETS = os.environ.get("AB_ASSETS", "C:/Project/MiniEngine/assets")
FRAMES = int(os.environ.get("AB_FRAMES", "90"))
SIZE = [int(v) for v in os.environ.get("AB_SIZE", "1280x720").split("x")]
FIXTURES = os.path.join(ROOT, "tests", "fixtures", "render_scenes", "scenes")
SHADERS = {
    "glsl": os.environ.get("AB_REF_SPV") or os.path.join(ROOT, "out", "slang", "ref_spv"),
    "slang": os.path.join(BUILD, "shaders"),
}
# AB_MODE=exe: A is the frozen baseline build (out/baseline_src, a worktree of the commit before the
# change, built with the same preset) with its own shaders, B the current build.
BASELINE = os.environ.get("AB_BASELINE_BUILD") or os.path.join(ROOT, "out", "baseline_src", "out", "build", PRESET)
EXES = {
    "glsl": (EXE, SHADERS["glsl"]),
    "slang": (EXE, SHADERS["slang"]),
    "base": (os.path.join(BASELINE, APP), os.path.join(BASELINE, "shaders")),
    "cur": (EXE, SHADERS["slang"]),
}
MODE = os.environ.get("AB_MODE", "shaders")
REUSE_A = os.environ.get("AB_REUSE_A")
A, B = ("base", "cur") if MODE == "exe" else ("glsl", "slang")

ROLLING_ROAD_CAMERA = {"position": [4.0, 1.4, -4.0], "yaw": -123.7, "pitch": -4.0}
MATERIALS_CAMERA = {"position": [0.0, 2.4, 6.5], "yaw": -90.0, "pitch": -14.0}
TOON_CAMERA = {"position": [-1.7, 1.15, -9.7], "yaw": 0.0, "pitch": -5.0}
EMISSIVE_CAMERA = {"position": [0.0, 1.8, 13.0], "yaw": -90.0, "pitch": -6.0}

# DDGI converges over many frames at a pace that depends on when the ray scene is installed, so it
# is off except in its own cases.
NO_DDGI = {"ddgi": {"enabled": False}}
RT_ON = dict(NO_DDGI)
RT_OFF = {"hardware_ray_tracing": False, **NO_DDGI}
PT_ON = {"path_tracing": {"enabled": True, "restir": False}, **NO_DDGI}
PT_RESTIR = {"path_tracing": {"enabled": True, "restir": True}, **NO_DDGI}
DDGI = {}

CASES = {
    # Deferred + forward, RT effects (default), clouds, fog, toon, skinning, TAA, bloom.
    "road_clouds": ("rolling_road_sky.yaml", ROLLING_ROAD_CAMERA, 12.0, RT_ON, []),
    "road_rt": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, RT_ON, []),
    "road_ddgi": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, DDGI, []),
    "road_ddgi_view": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, {"gbuffer_view": 15}, []),
    "road_ddgi_ray": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, {"gbuffer_view": 14}, []),
    "road_ddgi_software": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, {"hardware_ray_tracing": False}, ["--software-rays"]),
    # Raster shadows, SSR, VBAO, screen-space GI.
    "plain_software": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, RT_OFF, ["--software-rays"]),
    "plain_rt": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, RT_ON, []),
    "road_raster": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, RT_OFF, []),
    # The compute BVH walk (ray_tracing_common's non-RAY_QUERY path) for DDGI and the lighting pass.
    "road_software": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, RT_OFF, ["--software-rays"]),
    "road_pt": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, PT_ON, []),
    "road_restir": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, PT_RESTIR, []),
    # Local point/spot/area lights (clusters, local shadow atlas, LTC), KHR material extensions.
    "materials_rt": ("materials.yaml", MATERIALS_CAMERA, 9.0, RT_ON, []),
    "materials_raster": ("materials.yaml", MATERIALS_CAMERA, 9.0, RT_OFF, []),
    "materials_pt": ("materials.yaml", MATERIALS_CAMERA, 9.0, PT_ON, []),
    # DLSS super resolution, and ray reconstruction (dlss_rr_guides.comp, dlss_motion_vectors.comp).
    "road_dlss": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, {**RT_ON, "dlss_mode": 2}, []),
    "road_dlss_rr": ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, {**RT_ON, "dlss_mode": 2, "dlss_ray_reconstruction": True}, []),
    # The toon-shaded driver through the side window.
    "toon": ("rolling_road_fog.yaml", TOON_CAMERA, 12.0, RT_ON, []),
    # Emissive triangle NEE, the light grid.
    "emissive_pt": ("emissive_test.yaml", EMISSIVE_CAMERA, 5.0, PT_ON, []),
    "emissive_restir": ("emissive_test.yaml", EMISSIVE_CAMERA, 5.0, PT_RESTIR, []),
}
# The repository's own render scenes (absolute paths: os.path.join keeps them as they are).
CORNELL_CAMERA = {"position": [0.0, 0.0, -4.0], "yaw": -90.0, "pitch": 0.0}
SPHERES_CAMERA = {"position": [0.0, 0.6, 3.0], "yaw": -90.0, "pitch": -8.0}
TRACK_CAMERA = {"position": [0.0, 1.6, 30.0], "yaw": -90.0, "pitch": -3.0}
CASES.update({
    "fixture_cornell_rt": (os.path.join(FIXTURES, "cornell_box.yaml"), CORNELL_CAMERA, 9.0, RT_ON, []),
    "fixture_cornell_raster": (os.path.join(FIXTURES, "cornell_box.yaml"), CORNELL_CAMERA, 9.0, RT_OFF, []),
    "fixture_cornell_ddgi": (os.path.join(FIXTURES, "cornell_box.yaml"), CORNELL_CAMERA, 9.0, DDGI, []),
    "fixture_spheres_sun": (os.path.join(FIXTURES, "smooth_spheres_sun.yaml"), SPHERES_CAMERA, 12.0, RT_ON, []),
    "fixture_spheres_lamp": (os.path.join(FIXTURES, "smooth_spheres_lamp_sized.yaml"), SPHERES_CAMERA, 6.0, RT_OFF, []),
    "fixture_area_light": (os.path.join(FIXTURES, "area_light_floor.yaml"), MATERIALS_CAMERA, 6.0, RT_OFF, []),
    "fixture_iridescence": (os.path.join(FIXTURES, "iridescence.yaml"), SPHERES_CAMERA, 12.0, RT_OFF, []),
    "fixture_texture_transforms": (os.path.join(FIXTURES, "texture_transforms_unlit.yaml"), SPHERES_CAMERA, 12.0, RT_OFF, []),
    "fixture_track_rt": (os.path.join(FIXTURES, "ddgi_track.yaml"), TRACK_CAMERA, 12.0, RT_ON, []),
    "fixture_track_pt": (os.path.join(FIXTURES, "ddgi_track.yaml"), TRACK_CAMERA, 12.0, PT_ON, []),
})
for _view in (1, 2, 4, 5, 7, 13, 14, 15, 17, 18):
    CASES[f"road_view{_view}"] = ("rolling_road_fog.yaml", ROLLING_ROAD_CAMERA, 12.0, {"gbuffer_view": _view, **NO_DDGI}, [])


def write_state(case, scene, camera, ev, render, path):
    lines = [
        "version: 1",
        f"scene: {os.path.join(SCENES, scene).replace(os.sep, '/')}",
        f"viewport_size: [{SIZE[0]}, {SIZE[1]}]",
        "camera:",
        f"  position: [{camera['position'][0]}, {camera['position'][1]}, {camera['position'][2]}]",
        f"  yaw_degrees: {camera['yaw']}",
        f"  pitch_degrees: {camera['pitch']}",
        "  fov_degrees: 60",
        f"  exposure_ev100: {ev}",
        "  auto_exposure:",
        "    enabled: false",
        "  auto_white_balance:",
        "    enabled: false",
        "render_debug:",
    ]
    for key, value in render.items():
        if isinstance(value, dict):
            lines.append(f"  {key}:")
            for k, v in value.items():
                lines.append(f"    {k}: {json.dumps(v)}")
        else:
            lines.append(f"  {key}: {json.dumps(value)}")
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def run(case, variant, tag):
    scene, camera, ev, render, extra = CASES[case]
    os.makedirs(OUT, exist_ok=True)
    state = os.path.join(OUT, f"{case}.state.yaml")
    write_state(case, scene, camera, ev, render, state)
    capture = os.path.join(OUT, f"{case}_{tag}.png")
    log = os.path.join(OUT, f"{case}_{tag}.log")
    env = dict(os.environ)
    env.update({
        "MINIENGINE_HIDDEN_WINDOW": "1",
        "MINIENGINE_FIXED_FRAME_SECONDS": "0.0166667",
        "MINIENGINE_VIRTUAL_DESKTOP": "launcher",
        "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_RAWINPUT": "0", "SDL_JOYSTICK_WGI": "0",
        "SDL_XINPUT_ENABLED": "0", "SDL_JOYSTICK_DIRECTINPUT": "0", "SDL_JOYSTICK_GAMEINPUT": "0",
    })
    exe, shaders = EXES[variant]
    cmd = [exe, "--project", ROOT, "--assets", ASSETS, "--shaders", shaders, "--state", state,
           "--wait-for-scene", "--frames", str(FRAMES), "--capture", capture, "--no-audio"] + extra
    with open(log, "w", encoding="utf-8") as f:
        r = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, env=env, timeout=900)
    if r.returncode != 0 or not os.path.exists(capture):
        raise RuntimeError(f"{case} {tag}: exit {r.returncode}, see {log}")
    return capture


def run_a(case, tag):
    if REUSE_A:
        earlier = os.path.join(REUSE_A, f"{case}_{tag}.png")
        if os.path.exists(earlier):
            return earlier
    return run(case, A, tag)


def diff(a, b):
    x = np.asarray(Image.open(a).convert("RGB"), dtype=np.int16)
    y = np.asarray(Image.open(b).convert("RGB"), dtype=np.int16)
    d = np.abs(x - y)
    per_pixel = d.max(axis=2)
    return {
        "mean": float(d.mean()),
        "max": int(d.max()),
        "over2": float((per_pixel > 2).mean() * 100.0),
        "over8": float((per_pixel > 8).mean() * 100.0),
    }, d


def main():
    cases = sys.argv[1:] or list(CASES)
    results = {}
    for case in cases:
        a = run_a(case, A)
        a2 = run_a(case, A + "2")
        b = run(case, B, B)
        floor, _ = diff(a, a2)
        delta, d = diff(a, b)
        heat = np.clip(d.max(axis=2) * 8, 0, 255).astype(np.uint8)
        Image.fromarray(heat).save(os.path.join(OUT, f"{case}_diff.png"))
        results[case] = {"floor": floor, "slang": delta}
        print(f"{case:18s} floor mean {floor['mean']:.3f} max {floor['max']:3d} >2 {floor['over2']:.2f}%   "
              f"{B} mean {delta['mean']:.3f} max {delta['max']:3d} >2 {delta['over2']:.2f}% >8 {delta['over8']:.2f}%",
              flush=True)
    json.dump(results, open(os.path.join(OUT, "results.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
