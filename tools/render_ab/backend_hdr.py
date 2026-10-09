"""HDR10 output on Vulkan and D3D12: runs an ab.py case with hdr_output on, captures the presented
window (MINIENGINE_CAPTURE_WINDOW, PQ code values) on both, and compares them. D3D12 is forced to HDR10
(MINIENGINE_FORCE_HDR10) since the display may be in SDR mode. usage: python tools/render_ab/backend_hdr.py <case>"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("AB_OUT", "out/backend_hdr")
import ab
from PIL import Image, ImageChops
base = sys.argv[1] if len(sys.argv) > 1 else "road_rt"
scene, camera, ev, render, extra = ab.CASES[base]
name = base + "_hdr"
ab.CASES[name] = (scene, camera, ev, {**render, "hdr_output": True}, extra)
out = {}
for tag, backend in (("vk", "vulkan"), ("dx", "d3d12")):
    window = os.path.abspath(os.path.join(ab.OUT, f"{name}_{tag}_window.png"))
    os.environ["MINIENGINE_CAPTURE_WINDOW"] = window
    os.environ["MINIENGINE_FORCE_HDR10"] = "1"
    os.environ["AB_BACKEND"] = backend
    ab.run(name, "cur", tag)
    out[tag] = window
a = Image.open(out["vk"]).convert("RGB"); b = Image.open(out["dx"]).convert("RGB")
d = ImageChops.difference(a, b)
print(name, "window pixels differing >8:", sum(1 for p in d.convert("L").get_flattened_data() if p > 8), "bbox", d.getbbox())
