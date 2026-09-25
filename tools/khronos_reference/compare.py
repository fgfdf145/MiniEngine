#!/usr/bin/env python3
"""Puts each engine capture beside the Sample Viewer's and their difference, and prints the mean
difference per scene. Writes assets/khronos/captures/compare.html (and diff/<name>.png).

  --jpeg   embeds JPEG copies (made with macOS sips) instead of the PNGs, for a page small enough
           to publish."""

import base64
import html
import subprocess
import sys
import tempfile
from pathlib import Path

import png
from common import CAPTURES, MODELS_BY_FEATURE
from make_scenes import scene_names

DIFF_GAIN = 4  # the difference image is amplified so small shifts show


def compare(engine_path, viewer_path, diff_path):
    width, height, engine = png.read(engine_path)
    viewer_width, viewer_height, viewer = png.read(viewer_path)
    if (width, height) != (viewer_width, viewer_height):
        raise ValueError(f"{engine_path.name}: engine {width}x{height}, viewer {viewer_width}x{viewer_height}")
    total, diff_rows = 0, []
    for engine_row, viewer_row in zip(engine, viewer):
        diff = bytearray(len(engine_row))
        for i, (a, b) in enumerate(zip(engine_row, viewer_row)):
            d = a - b if a > b else b - a
            total += d
            diff[i] = min(255, d * DIFF_GAIN)
        diff_rows.append(diff)
    png.write(diff_path, width, height, diff_rows)
    return total / (width * height * 3)


def embed(path, jpeg):
    if not jpeg:
        return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()
    with tempfile.TemporaryDirectory() as directory:
        out = Path(directory) / "image.jpg"
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85", str(path), "--out", str(out)],
                       check=True, capture_output=True)
        return "data:image/jpeg;base64," + base64.b64encode(out.read_bytes()).decode()


def feature_of(model):
    return next(feature for feature, models in MODELS_BY_FEATURE.items() if model in models)


def main():
    jpeg = "--jpeg" in sys.argv
    (CAPTURES / "diff").mkdir(parents=True, exist_ok=True)
    rows = []
    for name, model, variant in scene_names():
        engine_path = CAPTURES / "engine" / f"{name}.png"
        viewer_path = CAPTURES / "viewer" / f"{name}.png"
        if not engine_path.exists() or not viewer_path.exists():
            missing = [label for label, path in (("engine", engine_path), ("viewer", viewer_path)) if not path.exists()]
            print(f"{name:40s} missing {', '.join(missing)}")
            continue
        diff_path = CAPTURES / "diff" / f"{name}.png"
        mean = compare(engine_path, viewer_path, diff_path)
        print(f"{name:40s} mean difference {mean:5.1f} / 255")
        rows.append((name, feature_of(model), mean, [embed(p, jpeg) for p in (viewer_path, engine_path, diff_path)]))

    body = []
    for name, feature, mean, images in rows:
        cells = "".join(f'<figure><img src="{src}" alt="{label} of {html.escape(name)}"><figcaption>{label}</figcaption></figure>'
                        for label, src in zip(("Khronos Sample Viewer", "MiniEngine", f"Difference x{DIFF_GAIN}"), images))
        body.append(f'<section><h2>{html.escape(name)} <small>{feature} · mean difference {mean:.1f}/255</small></h2>'
                    f'<div class="row">{cells}</div></section>')
    page = PAGE.replace("{{BODY}}", "\n".join(body))
    out = CAPTURES / "compare.html"
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out}")


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Khronos Reference Comparison</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --card: #ffffff; --line: #deded8; }
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { --bg: #151514; --fg: #ecece8; --muted: #9a9a94; --card: #1f1f1d; --line: #33332f; }
}
:root[data-theme="dark"] { --bg: #151514; --fg: #ecece8; --muted: #9a9a94; --card: #1f1f1d; --line: #33332f; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.5 system-ui, sans-serif; }
main { max-width: 1400px; margin: 0 auto; padding: 24px 16px 64px; }
h1 { font-size: 24px; margin: 0 0 4px; }
.lede { color: var(--muted); margin: 0 0 24px; max-width: 70ch; }
section { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px 14px; margin: 0 0 16px; }
h2 { font-size: 16px; margin: 0 0 10px; }
h2 small { color: var(--muted); font-weight: 400; margin-left: 8px; }
.row { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
figure { margin: 0; }
img { width: 100%; height: auto; display: block; border-radius: 6px; background: #000; }
figcaption { color: var(--muted); font-size: 13px; margin-top: 4px; }
@media (max-width: 700px) { .row { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<main>
<h1>Khronos reference comparison</h1>
<p class="lede">Khronos glTF-Sample-Assets models rendered by the Khronos glTF Sample Viewer (left) and by MiniEngine's
Khronos reference view (middle): the same Cannon_Exterior environment, exposure 1.0, Khronos PBR Neutral tone mapping and the
viewer's camera framing. The right column is their absolute difference, amplified. The viewer blurs its background; the
engine does not, so differences in the background are expected.</p>
{{BODY}}
</main>
</body>
</html>
"""

if __name__ == "__main__":
    main()
