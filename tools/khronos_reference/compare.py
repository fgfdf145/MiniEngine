#!/usr/bin/env python3
"""Puts each engine capture beside the Sample Viewer's and their difference, and prints the mean
difference per scene. Writes assets/khronos/captures/compare.html (and diff/<name>.png).

  --jpeg   embeds JPEG copies (made with macOS sips) instead of the PNGs, for a page small enough
           to publish.

The page is written without <html>/<head>/<body>: browsers add them, and the claude.ai artifact
host wraps the page in its own."""

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
        # Half size: three full-resolution images per scene would outgrow a publishable page.
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85", "-Z", "667", str(path), "--out", str(out)],
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

    page = render_page(rows)
    out = CAPTURES / "compare.html"
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out}")


# Scenes whose difference is understood, with the reason, shown beside their numbers.
KNOWN_DIFFERENCES = {
    "IORTestGrid": "The glass spheres magnify what is behind them about 3.7 times. The viewer magnifies its scene copy "
                   "with nearest filtering (hard, stair-stepped edges), the engine bilinearly (soft edges), and the "
                   "engine's geometric specular AA widens the small spheres' reflections. Not explained yet: the first "
                   "column's spheres are darker in the engine.",
    "DispersionTest": "KHR_materials_dispersion is phase 4b: the engine refracts every channel alike.",
    "DragonDispersion": "KHR_materials_dispersion is phase 4b: the engine refracts every channel alike.",
    "CompareDispersion": "KHR_materials_dispersion is phase 4b: the engine refracts every channel alike.",
    "DiffuseTransmissionTest": "KHR_materials_diffuse_transmission is phase 4c.",
    "DiffuseTransmissionTeacup": "KHR_materials_diffuse_transmission is phase 4c.",
    "UnlitTest": "The viewer shows unlit colours without tone mapping; the engine tone maps them on purpose.",
}
NOISE_FLOOR = 5.0  # mean difference of scenes that match; TAA, MSAA and filtering differ


def status_of(name, mean):
    if mean <= NOISE_FLOOR:
        return "match", "Matches"
    if name in KNOWN_DIFFERENCES:
        return "known", "Known gap"
    return "open", "Unexplained"


def render_page(rows):
    scale = max([mean for _, _, mean, _ in rows] + [NOISE_FLOOR * 2])
    summary, sections = [], []
    for name, feature, mean, images in rows:
        state, label = status_of(name, mean)
        anchor = "".join(c if c.isalnum() or c in "-_" else "-" for c in name)
        note = KNOWN_DIFFERENCES.get(name, "")
        summary.append(
            f'<tr><td><a href="#{anchor}">{html.escape(name)}</a></td><td class="feature">{feature.replace("_", " ")}</td>'
            f'<td class="num">{mean:.1f}</td><td class="bar"><span style="width:{100 * mean / scale:.1f}%" class="{state}"></span></td>'
            f'<td><span class="chip {state}">{label}</span></td></tr>')
        cells = "".join(
            f'<figure><img src="{src}" alt="{caption} rendering of {html.escape(name)}" loading="lazy"><figcaption>{caption}</figcaption></figure>'
            for caption, src in zip(("Khronos Sample Viewer", "MiniEngine", f"Difference ×{DIFF_GAIN}"), images))
        sections.append(
            f'<section id="{anchor}"><header><h2>{html.escape(name)}</h2><span class="chip {state}">{label}</span>'
            f'<span class="meta">{feature.replace("_", " ")} · mean difference <b>{mean:.1f}</b> / 255</span></header>'
            + (f'<p class="note">{html.escape(note)}</p>' if note else "")
            + f'<div class="row">{cells}</div></section>')
    matched = sum(1 for name, _, mean, _ in rows if mean <= NOISE_FLOOR)
    return (PAGE.replace("{{COUNT}}", str(len(rows))).replace("{{MATCHED}}", str(matched))
            .replace("{{FLOOR}}", f"{NOISE_FLOOR:.0f}").replace("{{SUMMARY}}", "\n".join(summary))
            .replace("{{SECTIONS}}", "\n".join(sections)))


PAGE = """<title>MiniEngine vs Sample Viewer</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">
<style>
:root {
  --bg: #f3f5f7; --surface: #ffffff; --fg: #17202a; --muted: #5d6b78; --line: #d9dfe5;
  --accent: #0f766e; --match: #2f855a; --known: #b7791f; --open: #c53030; --bar: #e6eaee;
  --sans: "IBM Plex Sans", system-ui, -apple-system, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, "SF Mono", Menlo, monospace;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --bg: #11161b; --surface: #182028; --fg: #e6ebf0; --muted: #93a1ae; --line: #2a3541;
    --accent: #2dd4bf; --match: #68d391; --known: #f6c35b; --open: #fc8181; --bar: #25303b;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --bg: #11161b; --surface: #182028; --fg: #e6ebf0; --muted: #93a1ae; --line: #2a3541;
  --accent: #2dd4bf; --match: #68d391; --known: #f6c35b; --open: #fc8181; --bar: #25303b;
}
body { background: var(--bg); color: var(--fg); font: 15px/1.55 var(--sans); }
main { max-width: 1320px; margin: 0 auto; padding-inline: 16px; padding-block: 32px 64px; display: grid; gap: 28px; }
h1 { font-size: 28px; font-weight: 600; margin: 0; text-wrap: balance; letter-spacing: -0.01em; }
.lede { color: var(--muted); margin: 6px 0 0; max-width: 72ch; }
.conditions { display: flex; flex-wrap: wrap; gap: 8px; margin: 14px 0 0; padding: 0; list-style: none; }
.conditions li { font: 12.5px/1 var(--mono); color: var(--muted); border: 1px solid var(--line); border-radius: 4px; padding: 6px 8px; background: var(--surface); }
.summary { background: var(--surface); border: 1px solid var(--line); border-radius: 8px; overflow-x: auto; }
.summary h2 { font-size: 15px; margin: 0; padding: 14px 16px 0; }
.summary p { margin: 2px 0 0; padding: 0 16px; color: var(--muted); font-size: 13.5px; }
table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 14px; }
th, td { text-align: left; padding: 7px 16px; border-top: 1px solid var(--line); white-space: nowrap; }
th { font-size: 11.5px; text-transform: uppercase; letter-spacing: 0.06em; color: var(--muted); font-weight: 500; }
td a { color: var(--fg); text-decoration: none; font-weight: 500; }
td a:hover, td a:focus-visible { color: var(--accent); text-decoration: underline; }
.feature { color: var(--muted); }
.num { font-family: var(--mono); font-variant-numeric: tabular-nums; text-align: right; }
.bar { width: 40%; min-width: 120px; }
.bar span { display: block; height: 8px; border-radius: 2px; background: var(--match); }
.bar span.known { background: var(--known); } .bar span.open { background: var(--open); }
td.bar { background-image: linear-gradient(var(--bar), var(--bar)); background-size: calc(100% - 32px) 8px; background-position: 16px center; background-repeat: no-repeat; }
.chip { display: inline-block; font-size: 11.5px; font-weight: 500; letter-spacing: 0.03em; padding: 2px 8px; border-radius: 999px; border: 1px solid currentColor; }
.chip.match { color: var(--match); } .chip.known { color: var(--known); } .chip.open { color: var(--open); }
section { display: grid; gap: 10px; scroll-margin-top: 16px; }
section header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 6px 12px; }
section h2 { font-size: 17px; margin: 0; font-weight: 600; }
.meta { color: var(--muted); font-size: 13.5px; }
.meta b { font-family: var(--mono); font-weight: 500; color: var(--fg); }
.note { margin: 0; font-size: 14px; max-width: 80ch; }
.row { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
figure { margin: 0; display: grid; gap: 4px; }
img { width: 100%; height: auto; display: block; border-radius: 4px; background: #000; aspect-ratio: 1334 / 1082; }
figcaption { color: var(--muted); font-size: 12.5px; }
@media (max-width: 720px) { .row { grid-template-columns: 1fr; } .bar { min-width: 80px; } }
</style>
<main>
<div>
  <h1>MiniEngine vs Khronos Sample Viewer</h1>
  <p class="lede">Khronos glTF-Sample-Assets test models rendered by the Khronos glTF Sample Viewer and by MiniEngine's Khronos
  reference view under the same conditions. {{MATCHED}} of {{COUNT}} scenes match to within the noise floor. The difference image
  is the absolute difference, amplified four times.</p>
  <ul class="conditions">
    <li>Cannon_Exterior, viewer rotation 90°</li><li>exposure 1.0</li><li>Khronos PBR Neutral</li>
    <li>45° vertical FOV, viewer framing</li><li>background prefiltered at roughness 0.6</li><li>1334 × 1082 pixels, shown at half size</li>
  </ul>
</div>
<div class="summary">
  <h2>Mean difference per scene</h2>
  <p>0 to 255 over RGB. Scenes at or below {{FLOOR}} match: the rest of the difference is anti-aliasing and texture filtering.</p>
  <table>
    <thead><tr><th>Scene</th><th>Feature</th><th class="num">Mean</th><th>Difference</th><th>Status</th></tr></thead>
    <tbody>
{{SUMMARY}}
    </tbody>
  </table>
</div>
{{SECTIONS}}
</main>
"""

if __name__ == "__main__":
    main()
