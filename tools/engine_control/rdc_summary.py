"""Summarizes a RenderDoc capture without the RenderDoc UI.

    python tools/engine_control/rdc_summary.py CAPTURE.rdc [--json] [--keep-xml] [--calls]

Converts the capture to RenderDoc's XML (renderdoccmd convert) and walks its API calls: the frame's
passes, split at the debug markers the engine leaves when RenderDoc is loaded (VulkanGpuTimer::Mark,
"end: <pass>"), each with its draws, dispatches, ray dispatches, barriers and pipeline changes, plus
the calls the capture holds most and the resources it named. --calls lists the calls of every pass.
The XML of a large frame runs to a gigabyte; it is deleted afterwards unless --keep-xml.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from collections import Counter
from pathlib import Path

RENDERDOCCMD = os.environ.get("RENDERDOCCMD", r"C:\Program Files\RenderDoc\renderdoccmd.exe")

MARKER_WORDS = ("BeginEvent", "SetMarker", "BeginDebugUtilsLabel", "InsertDebugUtilsLabel", "DebugMarkerBegin", "DebugMarkerInsert")
END_WORDS = ("EndEvent", "EndDebugUtilsLabel", "DebugMarkerEnd")


def kind_of(name: str) -> str | None:
    """Which counter an API call adds to."""
    short = name.split("::")[-1]
    if "DispatchRays" in short or "TraceRays" in short:
        return "ray_dispatches"
    if short.startswith("Dispatch") or short.startswith("vkCmdDispatch") or short == "ExecuteIndirect":
        return "dispatches"
    if short.startswith("Draw") or short.startswith("vkCmdDraw"):
        return "draws"
    if "Barrier" in short:
        return "barriers"
    if short in ("SetPipelineState", "SetPipelineState1", "vkCmdBindPipeline"):
        return "pipelines"
    if "BuildRaytracingAccelerationStructure" in short or "BuildAccelerationStructures" in short:
        return "acceleration_builds"
    if (short.startswith("Clear") and short != "ClearState") or short.startswith("vkCmdClear"):
        return "clears"
    if short.startswith("Copy") or short.startswith("vkCmdCopy") or short.startswith("vkCmdBlit"):
        return "copies"
    return None


def first_string(chunk) -> str:
    for element in chunk.iter():
        if element.tag == "string" and element.text:
            return element.text
    return ""


def summarize(xml_path: Path, list_calls: bool) -> dict:
    passes: list[dict] = []
    current = {"name": "(start)", "calls": Counter(), "list": []}
    totals = Counter()
    names: dict[str, str] = {}
    driver = None
    for event, element in ElementTree.iterparse(xml_path, events=("end",)):
        if element.tag == "driver":
            driver = element.text
        if element.tag != "chunk":
            continue
        name = element.get("name", "")
        totals[name] += 1
        if name.endswith("SetName") or name.endswith("SetDebugUtilsObjectNameEXT") or name.endswith("DebugMarkerSetObjectNameEXT"):
            label = first_string(element)
            if label:
                names[label] = name
        elif any(word in name for word in MARKER_WORDS):
            label = first_string(element) or name
            # A marker named "end: X" closes the pass X: what was recorded since the last marker.
            if label.startswith("end: "):
                current["name"] = label[5:]
                passes.append(current)
                current = {"name": "(after the last pass)", "calls": Counter(), "list": []}
            else:
                passes.append(current)
                current = {"name": label, "calls": Counter(), "list": []}
        elif not any(word in name for word in END_WORDS):
            kind = kind_of(name)
            if kind is not None:
                current["calls"][kind] += 1
            if list_calls and ("CommandList" in name or name.startswith("vkCmd")):
                current["list"].append(name.split("::")[-1])
        element.clear()
    passes.append(current)

    result_passes = []
    for entry in passes:
        if not entry["calls"] and not entry["list"]:
            continue
        item = {"name": entry["name"], **dict(entry["calls"])}
        if list_calls:
            item["calls"] = entry["list"]
        result_passes.append(item)
    return {
        "driver": driver,
        "passes": result_passes,
        "totals": dict(Counter({kind: sum(p.get(kind, 0) for p in result_passes) for kind in
                                ("draws", "dispatches", "ray_dispatches", "barriers", "pipelines", "acceleration_builds", "clears", "copies")})),
        "most_frequent_calls": totals.most_common(15),
        "named_resources": sorted(names)[:400],
        "named_resource_count": len(names),
    }


def convert(rdc: Path) -> Path:
    xml_path = rdc.with_suffix(".xml")
    subprocess.run([RENDERDOCCMD, "convert", "-f", str(rdc), "-c", "xml", "-o", str(xml_path)], check=True, capture_output=True)
    return xml_path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--keep-xml", action="store_true")
    parser.add_argument("--calls", action="store_true", help="list every pass's command list calls")
    options = parser.parse_args(argv)
    xml_path = options.capture if options.capture.suffix == ".xml" else convert(options.capture)
    try:
        result = summarize(xml_path, options.calls)
    finally:
        if xml_path != options.capture and not options.keep_xml:
            xml_path.unlink(missing_ok=True)
    if options.json:
        print(json.dumps(result, indent=2))
        return 0
    print(f"{result['driver']}: {len(result['passes'])} passes; totals {result['totals']}")
    for entry in result["passes"]:
        counts = ", ".join(f"{key} {value}" for key, value in entry.items() if key not in ("name", "calls"))
        print(f"  {entry['name']:<28} {counts}")
    print(f"{result['named_resource_count']} named resources")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
