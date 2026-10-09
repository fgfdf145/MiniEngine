"""Runs one ab.py case with extra render settings on Vulkan and on D3D12 (the dev build) and saves a
diff mask. usage: python tools/render_ab/backend_case.py <base case> <name> '<json render overrides>'"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("AB_OUT", "out/backend_case")
import ab
from PIL import Image, ImageChops
base, name, overrides = sys.argv[1], sys.argv[2], json.loads(sys.argv[3] if len(sys.argv) > 3 else "{}")
scene, camera, ev, render, extra = ab.CASES[base]
ab.CASES[name] = (scene, camera, ev, {**render, **overrides}, extra)
os.environ.pop("AB_BACKEND", None)
vk = ab.run(name, "cur", "vk")
os.environ["AB_BACKEND"] = "d3d12"
dx = ab.run(name, "cur", "dx")
a = Image.open(vk).convert("RGB"); b = Image.open(dx).convert("RGB")
d = ImageChops.difference(a, b)
mask = d.point(lambda v: 255 if v > 8 else 0)
mask.save(os.path.join(ab.OUT, f"{name}_mask.png"))
print(name, "differing pixels:", sum(1 for p in mask.convert("L").get_flattened_data() if p), "bbox", d.getbbox())
