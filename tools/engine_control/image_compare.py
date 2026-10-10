"""Compares two renders and says how much and where they differ.

    python tools/engine_control/image_compare.py A.png B.png [--out DIR] [--threshold 2] [--json]

Measures, on 8-bit sRGB images of the same size:
  psnr_db          peak signal-to-noise ratio over RGB (null when identical)
  mean_abs, max_abs  per-channel absolute difference, 0-255
  changed_share    share of pixels whose largest channel difference exceeds --threshold
  flip_mean        NVIDIA FLIP's mean perceptual error (0 same .. 1), when flip-evaluator is installed
  regions          the 8 tiles (of a 16x9 grid) with the largest mean difference, as pixel boxes
and writes DIR/diff_heat.png (the absolute difference, amplified, as a heat map), DIR/flip.png
(FLIP's magma map) and DIR/side_by_side.png (A | B | heat). Exits 0; the numbers are the result.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

try:
    import flip_evaluator  # pip install flip-evaluator
except ImportError:  # FLIP is optional; the other numbers do not need it
    flip_evaluator = None


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def heat_map(error: np.ndarray) -> np.ndarray:
    """error in 0..1 to a black-red-yellow-white ramp."""
    e = np.clip(error, 0.0, 1.0)[..., None]
    stops = np.array([[0, 0, 0], [180, 0, 0], [255, 200, 0], [255, 255, 255]], dtype=np.float32)
    position = e * (len(stops) - 1)
    low = np.floor(position).astype(int).clip(0, len(stops) - 2)
    t = position - low
    color = stops[low[..., 0]] * (1 - t) + stops[low[..., 0] + 1] * t
    return color.astype(np.uint8)


def compare(path_a: Path, path_b: Path, out: Path | None = None, threshold: int = 2, grid=(16, 9)) -> dict:
    a = load_rgb(path_a)
    b = load_rgb(path_b)
    if a.shape != b.shape:
        raise ValueError(f"The images differ in size: {a.shape[1]}x{a.shape[0]} and {b.shape[1]}x{b.shape[0]}")
    difference = np.abs(a.astype(np.int16) - b.astype(np.int16)).astype(np.float32)
    per_pixel = difference.max(axis=2)
    mse = float(np.mean(difference**2))
    result = {
        "a": str(path_a),
        "b": str(path_b),
        "size": [a.shape[1], a.shape[0]],
        "identical": bool(mse == 0.0),
        "psnr_db": None if mse == 0.0 else 10.0 * math.log10(255.0**2 / mse),
        "mean_abs": float(difference.mean()),
        "max_abs": int(difference.max()),
        "changed_share": float(np.mean(per_pixel > threshold)),
        "threshold": threshold,
    }

    height, width = per_pixel.shape
    columns, rows = grid
    tiles = []
    for row in range(rows):
        for column in range(columns):
            x0, x1 = column * width // columns, (column + 1) * width // columns
            y0, y1 = row * height // rows, (row + 1) * height // rows
            tile = per_pixel[y0:y1, x0:x1]
            if tile.size:
                tiles.append((float(tile.mean()), [x0, y0, x1, y1], float(np.mean(tile > threshold))))
    tiles.sort(key=lambda entry: entry[0], reverse=True)
    result["regions"] = [
        {"box": box, "mean_abs": round(mean, 3), "changed_share": round(share, 4)} for mean, box, share in tiles[:8] if mean > 0.0
    ]

    flip_map = None
    if flip_evaluator is not None:
        flip_map, flip_mean, _ = flip_evaluator.evaluate(
            a.astype(np.float32) / 255.0, b.astype(np.float32) / 255.0, "LDR", inputsRGB=True, applyMagma=True
        )
        result["flip_mean"] = float(flip_mean)
    else:
        result["flip_mean"] = None

    if out is not None:
        out.mkdir(parents=True, exist_ok=True)
        # A difference of 32/255 already shows white: renders that should match differ by a few levels.
        heat = heat_map(per_pixel / 32.0)
        Image.fromarray(heat).save(out / "diff_heat.png")
        Image.fromarray(np.concatenate([a, b, heat], axis=1)).save(out / "side_by_side.png")
        result["heat_map"] = str(out / "diff_heat.png")
        result["side_by_side"] = str(out / "side_by_side.png")
        if flip_map is not None:
            Image.fromarray((np.clip(flip_map, 0.0, 1.0) * 255.0).astype(np.uint8)).save(out / "flip.png")
            result["flip_map"] = str(out / "flip.png")
    return result


def summary(result: dict) -> str:
    if result["identical"]:
        return "identical"
    flip = f", FLIP {result['flip_mean']:.4f}" if result.get("flip_mean") is not None else ""
    return (
        f"PSNR {result['psnr_db']:.2f} dB{flip}, mean |d| {result['mean_abs']:.3f}, max |d| {result['max_abs']}, "
        f"{100.0 * result['changed_share']:.2f}% of pixels over {result['threshold']}"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("--out", type=Path, help="where to write the heat map, FLIP map and side by side")
    parser.add_argument("--threshold", type=int, default=2)
    parser.add_argument("--json", action="store_true", help="print every number as JSON")
    options = parser.parse_args(argv)
    result = compare(options.a, options.b, options.out, options.threshold)
    if options.json:
        print(json.dumps(result, indent=2))
    else:
        print(summary(result))
        for region in result["regions"][:3]:
            print(f"  box {region['box']}: mean |d| {region['mean_abs']}, {100 * region['changed_share']:.1f}% changed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
