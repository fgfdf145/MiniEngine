# Khronos Reference Comparison

Renders Khronos' `glTF-Sample-Assets` test models in MiniEngine and in the Khronos glTF Sample Viewer
under the same conditions and puts them side by side, so material work is accepted against the
Khronos implementation. Design: `docs/superpowers/specs/2026-09-26-khronos-reference-comparison-design.md`.

Everything it writes goes under `assets/` (not version controlled): models and the environment in
`assets/khronos/`, scenes in `assets/scenes/khronos/`, captures and the comparison page in
`assets/khronos/captures/`.

## 1. Models and scenes

```bash
python3 tools/khronos_reference/fetch.py
```

```bash
python3 tools/khronos_reference/make_scenes.py
```

`fetch.py` downloads the glTF flavour of every model listed in `common.py` (about 20 MB) and
`Cannon_Exterior.hdr`, keeping files already present; pass model names to fetch only those.
`make_scenes.py` writes one scene per model and one per material variant.

## 2. Engine captures

```bash
tools/khronos_reference/capture_engine.sh
```

Runs every scene with `--khronos-reference` (PBR Neutral, an HDRI texel exposed to 1, no glare, AO or
SSR, the viewer's framing, the viewer's blurred background) into `captures/engine/`. About 25 s per
scene. A run whose window was resized mid-way (macOS Stage Manager) is retried once.

## 3. Sample Viewer captures

The viewer is not scriptable from a shell, so its captures are taken in a browser:

1. `python3 tools/khronos_reference/capture_server.py` serves the Sample Viewer (proxied from its
   release site) on `http://127.0.0.1:8765/viewer/` and writes what the page posts to
   `captures/viewer/`. The page gets a `captureViewer(name)` function and a WebGL context that keeps
   its drawing buffer.
2. Size the browser viewport to the engine captures, 667x541 CSS pixels (the page scales the canvas
   to that size, and refuses a canvas of another aspect ratio).
3. For each model, open
   `http://127.0.0.1:8765/viewer/?model=https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/<Model>/glTF/<Model>.gltf`,
   wait for it to load, then run `await captureViewer("<Model>")` in the console. For a variant, click
   its radio button under Variants first and capture as `<Model>-<variant>`.

The page must be visible while it renders: a hidden browser tab stops drawing frames, and its canvas
keeps its initial 300x150 size until it does.

## 4. The comparison

```bash
python3 tools/khronos_reference/compare.py
```

Prints the mean absolute difference per scene (0 to 255, over RGB) and writes
`captures/compare.html`: viewer, engine and their difference amplified four times. `--jpeg` embeds
JPEG copies instead, for a page small enough to publish.

## Matching the Viewer

- **Framing** follows the viewer's `resetView`: each primitive's box grown to the cube around its
  bounding sphere, their union, looked at down -Z from where the larger of its x and y extents fits
  a 45 degree vertical FOV (`ComputeKhronosViewerExtents`, `Camera::FrameBoundsLikeKhronosViewer`).
- **Environment rotation**: the engine's 180 degrees is the viewer's default 90. Found by sweeping
  `CompareMetallic`: 0, 90, 180, 270 gave mean differences of 51, 16, 3.0 and 33.
- **Background**: the viewer shows the environment prefiltered at roughness 0.6; the reference view
  shows the engine's prefiltered map at the same roughness.
- **Exposure and tone mapping**: the viewer's exposure 1.0 and Khronos PBR Neutral.
