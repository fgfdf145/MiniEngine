"""Compile every shader output the renderer's CMakeLists lists from the Slang sources, in parallel,
into out/slang/spv; print each failure's first errors. Optional args: output names to build.

--dxil compiles the same list to DXIL instead (out/slang/dxil, shader model 6.8, MINIENGINE_DXIL
defined): the shaders' portability check for a future Direct3D 12 backend
(docs/design/2026-10-09-dxil-shader-portability-design.md)."""

import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SHADERS = os.path.join(ROOT, "shaders", "vulkan")
DXIL = "--dxil" in sys.argv[1:]
OUT = os.environ.get("SLANG_OUT") or os.path.join(ROOT, "out", "slang", "dxil" if DXIL else "spv")
STAGES = {"comp": "compute", "vert": "vertex", "frag": "fragment"}
# The slangc the CMake build uses (vcpkg's shader-slang, MINIENGINE_SLANGC_EXECUTABLE) when this checkout
# has it, so the checks see the compiler the engine ships with; otherwise the Vulkan SDK's. DXIL needs
# dxcompiler.dll on PATH (the Vulkan SDK's Bin has it).
VCPKG_SLANGC = os.path.join(ROOT, ".deps", "vcpkg_installed", "x64", "x64-windows", "tools", "shader-slang", "slangc.exe")
SLANGC = os.environ.get("SLANGC") or (VCPKG_SLANGC if os.path.exists(VCPKG_SLANGC) else
                                      os.path.join(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.357.0"), "Bin", "slangc.exe"))


def cmake_list(text, name):
    # Comments first: one may hold a ")" (path_trace_common.slang's in the define variants).
    text = "\n".join(line.split("#")[0] for line in text.splitlines())
    m = re.search(r"set\(" + name + r"\s+(.*?)\)", text, re.S)
    items = []
    for line in m.group(1).splitlines():
        line = line.strip()
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


def slangc(source, target, defines, path, reflection=None):
    cmd = [SLANGC, os.path.join(SHADERS, source + ".slang")] + target + ["-entry", "main",
           "-matrix-layout-column-major", "-warnings-disable", "41012"] + [f"-D{d}" for d in defines] + ["-o", path]
    if reflection:
        cmd += ["-reflection-json", reflection]
    return subprocess.run(cmd, capture_output=True, text=True)


def spirv_target(ray_query):
    return ["-target", "spirv", "-profile", "spirv_1_4" if ray_query else "spirv_1_3"]


def bindings(reflection):
    params = json.load(open(reflection, encoding="utf-8"))["parameters"]
    return {p["name"]: p["binding"] for p in params if "binding" in p}


def check_registers(output, vulkan, d3d):
    """Every resource's D3D register and space must be its Vulkan binding and set, which is what NVRHI's
    binding layouts give (zero binding offsets, registerSpaceIsDescriptorSet); push constants are b0
    (NVRHI's PushConstants(0)) in the space of the layout that carries them. Returns the problems."""
    problems = []
    for name, vk in vulkan.items():
        if vk["kind"] not in ("pushConstantBuffer", "descriptorTableSlot"):
            continue  # specialization constants
        dx = d3d.get(name)
        if dx is None:
            problems.append(f"{name}: not in the DXIL reflection")
        elif vk["kind"] == "pushConstantBuffer":
            if dx["kind"] != "constantBuffer" or dx["index"] != 0:
                problems.append(f"{name}: push constants at {dx['kind']} {dx['index']} space {dx.get('space', 0)}, want b0")
        elif vk["kind"] == "descriptorTableSlot":
            if (dx["index"], dx.get("space", 0)) != (vk["index"], vk.get("space", 0)):
                problems.append(f"{name}: Vulkan binding {vk['index']} set {vk.get('space', 0)}, D3D {dx['kind']} {dx['index']} space {dx.get('space', 0)}")
    # Two parameters on one register (a push constant block in a space a constant buffer uses).
    seen = {}
    for name, dx in d3d.items():
        key = (dx["kind"], dx["index"], dx.get("space", 0))
        if key in seen:
            problems.append(f"{name} and {seen[key]} share {dx['kind']} {dx['index']} space {dx.get('space', 0)}")
        seen[key] = name
    return problems


def compile_one(build):
    source, output, ray_query, defines = build
    defines = list(defines) + (["RAY_QUERY"] if ray_query else [])
    if not DXIL:
        r = slangc(source, spirv_target(ray_query), defines, os.path.join(OUT, output + ".spv"))
        return output, r.returncode, (r.stdout + r.stderr).strip()
    target = ["-target", "dxil", "-profile", "sm_6_8", "-stage", STAGES[source.rsplit(".", 1)[1]]]
    d3d_reflection = os.path.join(OUT, output + ".dxil.json")
    r = slangc(source, target, defines + ["MINIENGINE_DXIL"], os.path.join(OUT, output + ".dxil"), d3d_reflection)
    log = (r.stdout + r.stderr).strip()
    if r.returncode != 0:
        return output, r.returncode, log
    vulkan_reflection = os.path.join(OUT, output + ".spv.json")
    v = slangc(source, spirv_target(ray_query), defines, os.path.join(OUT, output + ".spv"), vulkan_reflection)
    if v.returncode != 0:
        return output, v.returncode, (v.stdout + v.stderr).strip()
    problems = check_registers(output, bindings(vulkan_reflection), bindings(d3d_reflection))
    if problems:
        return output, 1, "\n".join(f"error: register {p}" for p in problems)
    return output, 0, log


def unregistered_declarations():
    """Resource and push constant declarations without an explicit D3D register: Slang would place
    them by declaration order, differently in each shader."""
    missing = []
    for path in sorted(glob.glob(os.path.join(SHADERS, "*.slang"))):
        for number, line in enumerate(open(path, encoding="utf-8"), 1):
            code = line.split("//")[0]
            if ("vk::binding(" in code or "vk::push_constant" in code) and "register(" not in code and "D3D_REGISTER(" not in code and "D3D_PUSH_CONSTANTS(" not in code:
                missing.append(f"{os.path.basename(path)}:{number}: {line.strip()}")
    return missing


def main():
    os.makedirs(OUT, exist_ok=True)
    if DXIL:
        missing = unregistered_declarations()
        for line in missing:
            print(f"NO REGISTER {line}")
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
    if DXIL and missing:
        print(f"{len(missing)} declarations without a D3D register")
        failed += 1
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
