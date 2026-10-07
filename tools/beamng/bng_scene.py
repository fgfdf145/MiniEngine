"""A BeamNG level's objects: main/**/items.level.json, with prefabs expanded into their objects.

A level's scene is a tree of SimGroups saved one folder per group, each folder's items.level.json holding
one JSON object per line (class, __parent, position, rotationMatrix, scale, ...). A Prefab object names a
.prefab file (TorqueScript: nested `new Class(name) { field = "value"; };` blocks) or a .prefab.json
(lines like items.level.json); its objects are saved in the prefab's own space and placed by the
prefab's transform (Torque's Prefab::_updateChildTransform: world = prefab * child, the child's position
scaled by the prefab's scale first).

Objects come out as dicts with the JSON's keys plus "world": a 4x4 numpy matrix (BeamNG's space: metres,
Z up, X east, Y north) and "group": the chain of parent group names. Hidden objects and objects under a
hidden group are left out (BeamNG neither draws nor collides them).
"""

import json
import math
import re

import numpy as np

from bng_vfs import normalize


def _floats(value, count=None):
    if isinstance(value, str):
        value = [float(v) for v in value.split()]
    values = [float(v) for v in value]
    if count is not None and len(values) < count:
        raise ValueError("expected {} numbers, got {!r}".format(count, value))
    return values


def _truthy(value):
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true")
    return bool(value)


def object_matrix(obj):
    """Translation * rotation * scale of an object's position / rotationMatrix (or Torque's axis-angle
    `rotation`) / scale."""
    matrix = np.identity(4)
    if "rotationMatrix" in obj:
        # The nine numbers are the matrix column by column (each triple is where a local axis points).
        # Read row by row, gridmap_v2's ramp park clover falls apart; read as columns it matches the
        # level's minimap.
        matrix[:3, :3] = np.array(_floats(obj["rotationMatrix"], 9)).reshape(3, 3).T
    elif "rotation" in obj:
        x, y, z, angle = _floats(obj["rotation"], 4)
        axis = np.array([x, y, z])
        length = np.linalg.norm(axis)
        if length > 1e-9 and abs(angle) > 1e-9:
            matrix[:3, :3] = _axis_angle(axis / length, math.radians(angle))
    scale = _floats(obj.get("scale", [1, 1, 1]), 3)
    matrix[:3, :3] = matrix[:3, :3] @ np.diag(scale)
    matrix[:3, 3] = _floats(obj.get("position", [0, 0, 0]), 3)
    return matrix


def _axis_angle(axis, angle):
    x, y, z = axis
    c, s = math.cos(angle), math.sin(angle)
    t = 1 - c
    return np.array([
        [t * x * x + c, t * x * y - s * z, t * x * z + s * y],
        [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
        [t * x * z - s * y, t * y * z + s * x, t * z * z + c]])


_TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|(//[^\n]*)|([A-Za-z_$][\w$:.]*)|(-?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?)|(\S)')


def parse_torque_objects(text):
    """The objects a TorqueScript object file creates, as nested dicts: {"class", "name", fields...,
    "children": [...]}. Only the `new Class(name) { field = value; ... };` subset that the editor writes
    (a `$Var = new ...` assignment in front is skipped)."""
    tokens = []
    for match in _TOKEN.finditer(text):
        string, comment, word, number, symbol = match.groups()
        if comment is not None:
            continue
        if string is not None:
            tokens.append(("s", string.replace('\\"', '"').replace("\\\\", "\\")))
        elif word is not None:
            tokens.append(("w", word))
        elif number is not None:
            tokens.append(("s", number))
        else:
            tokens.append(("p", symbol))
    position = 0

    def peek(offset=0):
        return tokens[position + offset] if position + offset < len(tokens) else ("eof", "")

    def parse_new():
        nonlocal position
        position += 1  # new
        obj = {"class": peek()[1], "children": []}
        position += 1
        if peek() == ("p", "("):
            position += 1
            name = []
            while peek() != ("p", ")"):
                name.append(peek()[1])
                position += 1
            position += 1
            if name:
                obj["name"] = "".join(name)
        if peek() == ("p", "{"):
            position += 1
            while peek() != ("p", "}") and peek()[0] != "eof":
                if peek() == ("w", "new"):
                    obj["children"].append(parse_new())
                    continue
                key = peek()[1]
                position += 1
                if peek() == ("p", "["):  # field[index] = ...
                    while peek() != ("p", "]"):
                        position += 1
                    position += 1
                if peek() == ("p", "="):
                    position += 1
                    value = []
                    while peek() != ("p", ";") and peek()[0] != "eof":
                        value.append(peek()[1])
                        position += 1
                    obj[key] = " ".join(value)
                position += 1  # ;
            position += 1  # }
        if peek() == ("p", ";"):
            position += 1
        return obj

    objects = []
    while position < len(tokens):
        if peek() == ("w", "new"):
            objects.append(parse_new())
        else:
            position += 1
    return objects


class Level:
    def __init__(self, vfs, name):
        self.vfs = vfs
        self.name = name
        self.root = "/levels/{}".format(name)
        self.objects = []  # every non-group object, flattened, with "world" and "group"
        self.missing_prefabs = []
        self._load()

    def _load(self):
        prefix = normalize(self.root + "/main") + "/"
        files = sorted(key for key in self.vfs.entries if key.startswith(prefix) and key.endswith("/items.level.json"))
        by_name = {}
        records = []
        for key in files:
            for line in self.vfs.read_entry(key).decode("utf-8", "replace").splitlines():
                line = line.strip()
                if line:
                    record = json.loads(line)
                    records.append(record)
                    if record.get("class") in ("SimGroup", "SimSet") and "name" in record:
                        by_name[record["name"]] = record
        hidden_groups = {name for name, group in by_name.items() if _truthy(group.get("hidden", False))}

        def chain(record):
            names = []
            parent = record.get("__parent")
            seen = set()
            while parent and parent not in seen:
                seen.add(parent)
                names.append(parent)
                parent = by_name.get(parent, {}).get("__parent")
            return names

        for record in records:
            if record.get("class") in ("SimGroup", "SimSet"):
                continue
            groups = chain(record)
            if _truthy(record.get("hidden", False)) or any(g in hidden_groups for g in groups):
                continue
            world = object_matrix(record)
            self._add(record, world, groups, depth=0)

    def _add(self, record, world, groups, depth):
        if record.get("class") == "Prefab":
            if depth > 8:
                return
            children = self._prefab_children(record.get("filename", ""))
            if children is None:
                self.missing_prefabs.append(record.get("filename"))
                return
            # The child's position is scaled by the prefab's scale, its rotation and scale are not
            # (Prefab::_updateChildTransform convolves only the position, then mulL by objToWorld).
            prefab_scale = np.array(_floats(record.get("scale", [1, 1, 1]), 3))
            unscaled = object_matrix({k: v for k, v in record.items() if k != "scale"})
            name = record.get("name", record.get("filename", "prefab"))
            for child in children:
                local = object_matrix(child)
                local[:3, 3] *= prefab_scale
                child = dict(child)
                self._add(child, unscaled @ local, groups + [name], depth + 1)
            return
        record = dict(record)
        record["world"] = world
        record["group"] = groups
        self.objects.append(record)

    def _prefab_children(self, filename):
        """A prefab file's objects (groups flattened, hidden ones left out), or None if missing."""
        for candidate in (filename, filename + ".json", filename.replace(".prefab.json", ".prefab")):
            if candidate and self.vfs.exists(candidate):
                text = self.vfs.read(candidate).decode("utf-8", "replace")
                break
        else:
            return None
        result = []
        if text.lstrip().startswith("{"):
            for line in text.splitlines():
                line = line.strip()
                if line:
                    record = json.loads(line)
                    if record.get("class") not in ("SimGroup", "SimSet") and not _truthy(record.get("hidden", False)):
                        result.append(record)
            return result

        def walk(obj):
            if _truthy(obj.get("hidden", "0")):
                return
            if obj["class"] in ("SimGroup", "SimSet"):
                for child in obj["children"]:
                    walk(child)
            else:
                result.append(obj)

        for obj in parse_torque_objects(text):
            walk(obj)
        return result

    def of_class(self, name):
        return [o for o in self.objects if o.get("class") == name]
