"""Compile every shader output the renderer's CMakeLists lists from the Slang sources, in parallel,
into out/slang/spv; print each failure's first errors. Optional args: output names to build.

--dxil compiles the same list to DXIL instead (out/slang/dxil, shader model 6.8, MINIENGINE_DXIL
defined): the shaders' portability check for a future Direct3D 12 backend
(docs/design/2026-10-09-dxil-shader-portability-design.md)."""

import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SHADERS = os.path.join(ROOT, "shaders", "vulkan")
DXIL = "--dxil" in sys.argv[1:]
OUT = os.environ.get("SLANG_OUT") or os.path.join(ROOT, "out", "slang", "dxil" if DXIL else "spv")
STAGES = {"comp": "compute", "vert": "vertex", "frag": "fragment"}
SLANGC = os.environ.get("SLANGC") or os.path.join(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.357.0"), "Bin", "slangc.exe")


def cmake_list(text, name):
    m = re.search(r"set\(" + name + r"\s+(.*?)\)", text, re.S)
    items = []
    for line in m.group(1).splitlines():
        line = line.split("#")[0].strip()
        if line:
            items.extend(x.strip('"') for x in line.split())
    return items


def builds():
    text = open(os.path.join(ROOT, "engine", "renderer", "CMakeLists.txt"), encoding="utf-8").read()
    result = []
    for name in cmake_list(text, "MINIENGINE_SHADER_SOURCES"):
        result.append((name, name, False, []))
    for name in cmake_list(text, "MINIENGINE_RAY_QUERY_SHADERS"):
        stem, ext = name.rsplit(".", 1)
        result.append((name, f"{stem}_ray_query.{ext}", True, []))
    for name in cmake_list(text, "MINIENGINE_HARDWARE_RAY_SHADERS"):
        result.append((name, name, True, []))
    for variant in cmake_list(text, "MINIENGINE_SHADER_DEFINE_VARIANTS"):
        src, out, define = variant.split("|")
        result.append((src, out, False, [define]))
    for variant in cmake_list(text, "MINIENGINE_HARDWARE_RAY_DEFINE_VARIANTS"):
        src, out, defines = variant.split("|")
        result.append((src, out, True, defines.split(",")))
    if DXIL:
        # DXIL has no specialization constants (shaders/vulkan/specialization.slang): also compile
        # every shader that has them with all of them on.
        for src, out, ray_query, defines in list(result):
            text = open(os.path.join(SHADERS, src + ".slang"), encoding="utf-8").read()
            if "SPECIALIZATION_BOOL(" in text:
                on = [f"SPECIALIZATION_{i}=true" for i in range(4)]
                result.append((src, out + "_specialized", ray_query, defines + on))
    return result


def compile_one(build):
    source, output, ray_query, defines = build
    defines = list(defines) + (["RAY_QUERY"] if ray_query else [])
    if DXIL:
        target = ["-target", "dxil", "-profile", "sm_6_8", "-stage", STAGES[source.rsplit(".", 1)[1]]]
        defines.append("MINIENGINE_DXIL")
        path = os.path.join(OUT, output + ".dxil")
    else:
        target = ["-target", "spirv", "-profile", "spirv_1_4" if ray_query else "spirv_1_3"]
        path = os.path.join(OUT, output + ".spv")
    cmd = [SLANGC, os.path.join(SHADERS, source + ".slang")] + target + ["-entry", "main",
           "-matrix-layout-column-major", "-warnings-disable", "41012"] + [f"-D{d}" for d in defines] + ["-o", path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return output, r.returncode, (r.stdout + r.stderr).strip()


def main():
    os.makedirs(OUT, exist_ok=True)
    wanted = set(a for a in sys.argv[1:] if a != "--dxil")
    todo = [b for b in builds() if not wanted or b[1] in wanted or b[0] in wanted]
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for output, code, log in pool.map(compile_one, todo):
            if code != 0:
                failed += 1
                lines = [l for l in log.splitlines() if "error" in l][:6]
                print(f"FAIL {output}")
                for l in lines:
                    print("   ", l.replace(SHADERS + os.sep, ""))
            elif log:
                warnings = [l for l in log.splitlines() if "warning" in l]
                print(f"ok   {output} ({len(warnings)} warnings)")
            else:
                print(f"ok   {output}")
    print(f"{len(todo) - failed}/{len(todo)} compiled")


if __name__ == "__main__":
    main()
