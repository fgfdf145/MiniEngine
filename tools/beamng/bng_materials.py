"""BeamNG's material definitions (*.materials.json) and ground models (art/groundmodels.json).

A shape's mesh names a material by its "mapTo" (the name the modeller gave it); BeamNG looks the name up
among every Material it loaded: the level's own files first, then the shared /assets and /art ones.
Materials come in two generations: the older Torque one (colorMap / diffuseMap, specularMap, diffuseColor)
and version 1.5's PBR one (baseColorMap, roughnessMap, metallicMap, ambientOcclusionMap, normalMap).
Only Stages[0] (the first layer) is used here.
"""

import json
import re

from bng_vfs import normalize


def loads_lenient(text):
    """BeamNG's JSON: comments and trailing commas."""
    # Comments go, strings stay (a string may hold "//", as in a URL).
    text = re.sub(r'"(?:[^"\\]|\\.)*"|/\*.*?\*/|//[^\n]*', lambda m: m.group(0) if m.group(0).startswith('"') else "", text, flags=re.S)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


class MaterialLibrary:
    def __init__(self, vfs, level):
        self.vfs = vfs
        self.by_name = {}
        self.terrain = {}  # internalName -> TerrainMaterial
        level_prefix = normalize("/levels/{}/".format(level))
        files = [k for k in vfs.entries if k.endswith("materials.json")]
        # Shared ones first so that the level's own definitions win.
        files.sort(key=lambda k: (k.startswith(level_prefix), k))
        for key in files:
            if key.startswith("/levels/") and not key.startswith(level_prefix):
                continue
            try:
                data = loads_lenient(vfs.read_entry(key).decode("utf-8", "replace"))
            except ValueError:
                continue
            if not isinstance(data, dict):
                continue
            for name, material in data.items():
                if not isinstance(material, dict):
                    continue
                material = dict(material)
                material["_file"] = key
                if material.get("class") == "TerrainMaterial":
                    self.terrain[material.get("internalName", name)] = material
                elif material.get("class") in ("Material", "CustomMaterial", None):
                    self.by_name[material.get("name", name).lower()] = material
                    if material.get("mapTo"):
                        self.by_name[str(material["mapTo"]).lower()] = material

    def get(self, name):
        return self.by_name.get((name or "").lower())


class GroundModels:
    def __init__(self, vfs):
        data = loads_lenient(vfs.read("/art/groundmodels.json").decode("utf-8", "replace"))
        self.models = {}
        for name, model in data.items():
            self.models[name.upper()] = model
            for alias in model.get("aliases", []):
                self.models.setdefault(alias.upper(), model)
            self.models.setdefault(model.get("collisiontype", name).upper(), model)

    def get(self, name):
        return self.models.get((name or "ASPHALT").upper(), self.models["ASPHALT"])

    def name_of(self, name):
        model = self.get(name)
        return model.get("collisiontype", "ASPHALT").upper()
