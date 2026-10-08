"""Compare the SPIR-V interfaces of two shader builds (GLSL vs Slang).

usage: compare_reflection.py <reference_dir> <candidate_dir> [names...]

For every .spv in candidate_dir (or the names given) that also exists in reference_dir, reflect both
with spirv-cross and compare what the pipelines depend on: entry point and workgroup size, every
descriptor (set, binding, kind, array size, image format, block layout), push constants, spec
constants and stage input/output locations. Names are ignored; layouts are compared structurally.
Exit code 1 when any interface differs.
"""

import json
import os
import subprocess
import sys

SPIRV_CROSS = os.path.join(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.357.0"), "Bin", "spirv-cross.exe")
SPIRV_DIS = os.path.join(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.357.0"), "Bin", "spirv-dis.exe")


def module_facts(path):
    """Built-ins in the entry point's interface (by storage class), execution modes, capabilities
    and extended instruction sets."""
    dis = subprocess.run([SPIRV_DIS, path, "--raw-id"], capture_output=True, text=True, check=True).stdout
    builtin_of = {}
    storage_of = {}
    interface = set()
    modes = set()
    capabilities = set()
    decorations = {}
    member_position = set()
    member_invariant = set()
    for line in dis.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "OpDecorate" and len(parts) >= 4 and parts[2] == "BuiltIn":
            builtin_of[parts[1]] = parts[3]
        elif parts[0] == "OpMemberDecorate" and len(parts) >= 5 and parts[3] == "BuiltIn":
            # gl_PerVertex: only Position is ever written.
            if parts[4] == "Position":
                member_position.add((parts[1], parts[2]))
        elif parts[0] == "OpMemberDecorate" and len(parts) >= 4 and parts[3] == "Invariant":
            member_invariant.add((parts[1], parts[2]))
        elif parts[0] == "OpDecorate" and len(parts) >= 3:
            decorations.setdefault(parts[1], set()).add(parts[2])
        elif len(parts) >= 4 and parts[1] == "=" and parts[2] == "OpVariable":
            storage_of[parts[0]] = parts[4] if len(parts) > 4 else ""
        elif parts[0] == "OpEntryPoint":
            interface.update(p for p in parts[3:] if p.startswith("%"))
        elif parts[0] in ("OpExecutionMode", "OpExecutionModeId"):
            mode = parts[2]
            if mode != "LocalSize":
                modes.add(" ".join(parts[2:]) if mode not in ("LocalSize",) else mode)
        elif parts[0] == "OpCapability":
            capabilities.add(parts[1])
    builtins = set()
    for struct, index in member_position:
        builtins.add("Output Position" + (" Invariant" if (struct, index) in member_invariant else ""))
    for var, name in builtin_of.items():
        if var in interface or not interface:
            extra = ""
            if "Invariant" in decorations.get(var, set()):
                extra = " Invariant"
            builtins.add(f"{storage_of.get(var, '?')} {name}{extra}")
    return {"builtins": builtins, "modes": modes, "capabilities": capabilities}


def matrix_ops(path):
    """Counts of matrix arithmetic: a GLSL matrix product written as Slang `*` turns into an
    element-wise multiply, and shows up here as a missing OpMatrixTimesMatrix."""
    dis = subprocess.run([SPIRV_DIS, path], capture_output=True, text=True, check=True).stdout
    counts = {"MatrixTimesMatrix": 0, "MatrixVector": 0}
    for line in dis.splitlines():
        if "OpMatrixTimesMatrix" in line:
            counts["MatrixTimesMatrix"] += 1
        elif "OpMatrixTimesVector" in line or "OpVectorTimesMatrix" in line:
            counts["MatrixVector"] += 1
    return counts

RESOURCE_KINDS = [
    "ubos",
    "ssbos",
    "textures",
    "separate_images",
    "separate_samplers",
    "images",
    "acceleration_structures",
    "subpass_inputs",
]


def reflect(path):
    out = subprocess.run([SPIRV_CROSS, path, "--reflect"], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def matrix_dims(type_name):
    # mat4 -> (4, 4); mat4x3 -> (4 columns, 3 rows); dmat... not used.
    base = type_name.lstrip("d")
    if not base.startswith("mat"):
        return None
    dims = base[3:]
    if "x" in dims:
        c, r = dims.split("x")
        return int(c), int(r)
    return int(dims), int(dims)


def normalize_type(types, type_name, member=None):
    """A structural description of a (member) type, independent of names and of the way the
    compiler spells a matrix's majorness."""
    desc = {}
    if member is not None:
        if "array" in member:
            desc["array"] = member["array"]
        if "array_stride" in member:
            desc["array_stride"] = member["array_stride"]
        if "offset" in member:
            desc["offset"] = member["offset"]
        if "matrix_stride" in member:
            desc["matrix_stride"] = member["matrix_stride"]
    if type_name in types:
        t = types[type_name]
        if t["name"].startswith("_Array_std") and len(t["members"]) == 1 and member is not None:
            # Slang wraps a std140 array in a struct; the layout is the bare array's.
            inner = dict(t["members"][0])
            inner["offset"] = member.get("offset", 0)
            return normalize_type(types, inner["type"], inner)
        desc["struct"] = [normalize_type(types, m["type"], m) for m in t["members"]]
        return desc
    dims = matrix_dims(type_name)
    if dims is not None:
        columns, rows = dims
        if member is not None and member.get("row_major"):
            # SPIR-V RowMajor on an R x C matrix type: the same memory as a column-major C x R one.
            columns, rows = rows, columns
        desc["matrix"] = (columns, rows)
        return desc
    desc["type"] = type_name
    return desc


def resources(reflection):
    types = reflection.get("types", {})
    result = {}
    for kind in RESOURCE_KINDS:
        for r in reflection.get(kind, []):
            key = (r.get("set", 0), r.get("binding", 0))
            desc = {"kind": kind}
            if "array" in r:
                desc["array"] = r["array"]
            if "format" in r:
                desc["format"] = r["format"]
            if kind in ("ubos", "ssbos"):
                desc["block_size"] = r.get("block_size")
                desc["layout"] = normalize_type(types, r["type"])
            else:
                desc["type"] = r["type"]
            if kind == "ssbos":
                desc["_writable"] = not r.get("readonly", False)
            if key in result:
                result[key] = [result[key], desc]
            else:
                result[key] = desc
    return result


def push_constants(reflection):
    types = reflection.get("types", {})
    blocks = reflection.get("push_constants", [])
    return [normalize_type(types, b["type"]) for b in blocks]


def stage_io(reflection, kind):
    types = reflection.get("types", {})
    result = {}
    for v in reflection.get(kind, []):
        if "location" not in v:
            continue  # built-ins
        result[v["location"]] = {"type": v["type"], "array": v.get("array")}
    return result


def spec_constants(reflection):
    return {c["id"]: c["type"] for c in reflection.get("specialization_constants", [])}


def entry(reflection):
    eps = reflection.get("entryPoints", [])
    return [(e["name"], e["mode"], tuple(e.get("workgroup_size", []))) for e in eps]


def strip_soft(desc):
    if isinstance(desc, dict):
        return {k: strip_soft(v) for k, v in desc.items() if not k.startswith("_")}
    if isinstance(desc, list):
        return [strip_soft(v) for v in desc]
    return desc


def first_difference(a, b, path=""):
    if type(a) != type(b):
        return f"{path}: {a!r} vs {b!r}"
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b), key=str):
            if k not in a or k not in b:
                return f"{path}.{k}: {a.get(k, '<missing>')!r} vs {b.get(k, '<missing>')!r}"
            d = first_difference(a[k], b[k], f"{path}.{k}")
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: length {len(a)} vs {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_difference(x, y, f"{path}[{i}]")
            if d:
                return d
        return None
    return None if a == b else f"{path}: {a!r} vs {b!r}"


def fixed_block_as_array(a, b):
    """The reference's fixed-size storage block is the candidate's runtime array element."""
    if a.get("kind") != "ssbos" or b.get("kind") != "ssbos" or b.get("block_size") != 0:
        return False
    members = b["layout"].get("struct", [])
    if len(members) != 1 or members[0].get("array") != [0]:
        return False
    element = {k: v for k, v in members[0].items() if k not in ("array", "array_stride", "offset")}

    def unwrap(d):
        # A struct whose only member, at offset 0, is itself a struct: the same bytes.
        while "struct" in d and len(d["struct"]) == 1 and d["struct"][0].get("offset") == 0                 and "struct" in d["struct"][0] and "array" not in d["struct"][0]:
            d = {k: v for k, v in d["struct"][0].items() if k != "offset"}
        return d

    return unwrap(element) == unwrap(a["layout"]) and members[0].get("array_stride") == a["block_size"]


def compare(reference, candidate):
    errors = []
    warnings = []
    if entry(reference) != entry(candidate):
        errors.append(f"entry point: {entry(reference)} vs {entry(candidate)}")
    ref_res, cand_res = resources(reference), resources(candidate)
    for key in sorted(set(ref_res) | set(cand_res)):
        a, b = ref_res.get(key), cand_res.get(key)
        if a is None:
            # An extra binding the GLSL left unused is fine only if the layout has it; flag it.
            errors.append(f"set {key[0]} binding {key[1]}: only in candidate: {b}")
        elif b is None:
            # The reference declares it; the candidate dropped it as unused. Pipelines still bind it.
            warnings.append(f"set {key[0]} binding {key[1]}: not used by candidate ({a['kind'] if isinstance(a, dict) else a})")
        elif isinstance(a, dict) and isinstance(b, dict) and fixed_block_as_array(a, b):
            warnings.append(f"set {key[0]} binding {key[1]}: fixed-size block read as a one-element structured buffer")
        elif strip_soft(a) != strip_soft(b):
            errors.append(f"set {key[0]} binding {key[1]} ({a['kind'] if isinstance(a, dict) else '?'}): {first_difference(strip_soft(a), strip_soft(b))}")
        elif isinstance(a, dict) and a.get("_writable") != b.get("_writable"):
            warnings.append(f"set {key[0]} binding {key[1]}: writable {a.get('_writable')} vs {b.get('_writable')}")
    pa, pb = push_constants(reference), push_constants(candidate)
    if pa != pb:
        if not pa or not pb:
            (warnings if not pb else errors).append(f"push constants: {pa} vs {pb}")
        else:
            # The candidate may drop trailing unused members only if offsets agree for the rest.
            errors.append(f"push constants: {first_difference(pa, pb)}")
    sa, sb = spec_constants(reference), spec_constants(candidate)
    if sa != sb:
        # Constants the candidate does not use are dropped; the pipelines' map entries for them are
        # ignored. Any other difference is an error.
        if all(sa.get(k) == v for k, v in sb.items()):
            warnings.append(f"spec constants not used by candidate: {sorted(set(sa) - set(sb))}")
        else:
            errors.append(f"spec constants: {sa} vs {sb}")
    for kind in ("inputs", "outputs"):
        ia, ib = stage_io(reference, kind), stage_io(candidate, kind)
        if ia != ib:
            stage = entry(candidate)[0][1] if entry(candidate) else ""
            if kind == "inputs" and stage == "frag" and all(ia.get(k) == v for k, v in ib.items()):
                warnings.append(f"fragment inputs not used by candidate: {sorted(set(ia) - set(ib))}")
            else:
                errors.append(f"{kind}: {ia} vs {ib}")
    return errors, warnings


def main():
    ref_dir, cand_dir = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or sorted(n for n in os.listdir(cand_dir) if n.endswith(".spv"))
    failed = 0
    for name in names:
        ref_path, cand_path = os.path.join(ref_dir, name), os.path.join(cand_dir, name)
        if not os.path.exists(ref_path):
            print(f"{name}: no reference")
            continue
        if not os.path.exists(cand_path):
            print(f"{name}: no candidate")
            failed += 1
            continue
        errors, warnings = compare(reflect(ref_path), reflect(cand_path))
        fa, fb = module_facts(ref_path), module_facts(cand_path)
        if fa["builtins"] != fb["builtins"]:
            errors.append(f"built-ins: only ref {sorted(fa['builtins'] - fb['builtins'])}, only cand {sorted(fb['builtins'] - fa['builtins'])}")
        if fa["modes"] != fb["modes"]:
            errors.append(f"execution modes: only ref {sorted(fa['modes'] - fb['modes'])}, only cand {sorted(fb['modes'] - fa['modes'])}")
        extra_caps = fb["capabilities"] - fa["capabilities"]
        if extra_caps:
            warnings.append(f"capabilities only in candidate: {sorted(extra_caps)}")
        ma, mb = matrix_ops(ref_path), matrix_ops(cand_path)
        if ma != mb:
            warnings.append(f"matrix ops: {ma} vs {mb}")
        status = "FAIL" if errors else "ok"
        print(f"{name}: {status}")
        for e in errors:
            print(f"  error: {e}")
        for w in warnings:
            print(f"  warn:  {w}")
        failed += bool(errors)
    print(f"{len(names) - failed}/{len(names)} match")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
