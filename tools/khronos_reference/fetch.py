#!/usr/bin/env python3
"""Downloads the compared models (the glTF flavour of each) and the Sample Viewer's default
environment into assets/khronos/. Files already present are kept."""

import json
import sys
import urllib.request

import shutil

from common import (DERIVED_MODELS, ENVIRONMENT_FILE, ENVIRONMENT_URL, ENVIRONMENTS, MODELS, SAMPLE_ASSETS,
                    SAMPLE_ASSETS_RAW, all_models)


def download(url, destination):
    if destination.exists():
        return 0
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".part")
    with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
        out.write(response.read())
    partial.rename(destination)
    return destination.stat().st_size


def main():
    requested = set(sys.argv[1:]) or set(all_models())
    wanted = {DERIVED_MODELS[model][0] if model in DERIVED_MODELS else model for model in requested}
    tree_url = f"https://api.github.com/repos/{SAMPLE_ASSETS}/git/trees/main?recursive=1"
    with urllib.request.urlopen(tree_url) as response:
        tree = json.load(response)
    if tree.get("truncated"):
        sys.exit("the GitHub tree listing came back truncated")

    total = 0
    for entry in tree["tree"]:
        parts = entry["path"].split("/")
        if entry["type"] != "blob" or len(parts) < 4 or parts[0] != "Models":
            continue
        model, flavour = parts[1], parts[2]
        if model not in wanted or flavour != "glTF":
            continue
        relative = "/".join(parts[3:])
        written = download(f"{SAMPLE_ASSETS_RAW}/{entry['path']}", MODELS / model / relative)
        if written:
            print(f"{model}/{relative} ({written / 1e6:.1f} MB)")
        total += written

    total += download(ENVIRONMENT_URL, ENVIRONMENTS / ENVIRONMENT_FILE)
    # The derived models: the source's files, the .gltf edited and named after the model (the engine
    # finds a scene's model by its file name when the stored path does not resolve, which two files of
    # one name would make ambiguous). Rewritten every run.
    for model in sorted(requested & set(DERIVED_MODELS)):
        source, edit = DERIVED_MODELS[model]
        target = MODELS / model
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(MODELS / source, target, ignore=shutil.ignore_patterns("*.miniengine_asset.yaml"))
        for gltf in list(target.glob("*.gltf")):
            (target / f"{model}.gltf").write_text(edit(gltf.read_text(encoding="utf-8")), encoding="utf-8")
            if gltf.name != f"{model}.gltf":
                gltf.unlink()
        print(f"{model} derived from {source}")
    missing = [model for model in sorted(wanted | requested) if not any((MODELS / model).glob("*.gltf"))]
    print(f"downloaded {total / 1e6:.1f} MB")
    if missing:
        sys.exit("no glTF flavour found for: " + ", ".join(missing))


if __name__ == "__main__":
    main()
