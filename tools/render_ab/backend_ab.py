"""Every render_ab case on Vulkan and on Direct3D 12 (the dev build), with the share of pixels that
differ by more than 8 and each run's validation errors. usage: python tools/render_ab/backend_ab.py [case ...]"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("AB_OUT", "out/backend_ab")
import ab
import numpy as np
from PIL import Image

ERROR = re.compile(r"\[(error|critical)\]")
cases = sys.argv[1:] or list(ab.CASES)
for case in cases:
    images = {}
    errors = {}
    try:
        for tag, backend in (("vk", "vulkan"), ("dx", "d3d12")):
            os.environ["AB_BACKEND"] = backend
            images[tag] = ab.run(case, "cur", tag)
            log = os.path.splitext(images[tag])[0] + ".log"
            with open(log, encoding="utf-8", errors="replace") as f:
                errors[tag] = sum(1 for line in f if ERROR.search(line))
    except Exception as error:  # noqa: BLE001
        print(f"{case:28s} FAILED {error}", flush=True)
        continue
    a = np.asarray(Image.open(images["vk"]).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(images["dx"]).convert("RGB"), dtype=np.int16)
    d = np.abs(a - b).max(axis=2)
    print(f"{case:28s} >8 {100.0 * (d > 8).mean():6.2f}%  >32 {100.0 * (d > 32).mean():6.2f}%  mean {np.abs(a - b).mean():6.3f}"
          f"  errors vk {errors['vk']} dx {errors['dx']}", flush=True)
