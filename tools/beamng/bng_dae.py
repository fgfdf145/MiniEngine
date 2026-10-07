"""BeamNG / Torque3D shapes from COLLADA (.dae): the highest detail level's meshes and the collision meshes.

Torque reads a shape's level of detail from node names. A "baseXX" node holds a "startXX" node with the
geometry and marker nodes ("detail500", "collision-1", "nulldetail10", "LOS-9"); every mesh node under
start ends in the size of the detail level it belongs to ("wall_a500", "wall_a80", "Colmesh_wall-1").
The largest non-negative size is the highest detail, the one drawn up close; negative sizes are collision
("Col*" names) and line-of-sight meshes ("LOS*", left out here). A shape without a base node is one detail
level; there a mesh named Col* / Collision* collides and every other mesh draws.

Everything stays in the shape's own space, Torque's: Z up, metres (a Y-up file is turned to Z up, as Torque
does, and <unit meter> applied).
"""

import re
import xml.etree.ElementTree as ElementTree

import numpy as np


class Primitive:
    """One material's triangles: flat vertex arrays (positions, normals, uv0, uv1, colours may be None)
    and uint32 indices."""

    def __init__(self, material, positions, normals, uv0, uv1, colors, indices):
        self.material = material
        self.positions = positions
        self.normals = normals
        self.uv0 = uv0
        self.uv1 = uv1
        self.colors = colors
        self.indices = indices


class Shape:
    def __init__(self):
        self.primitives = []  # drawn, highest detail
        self.collision = []  # (positions (n,3), indices (m,), material name)
        self.detail_size = None
        self.materials = set()


def _strip(tag):
    return tag.rsplit("}", 1)[-1]


def _floats(text):
    return np.array(text.split(), dtype=np.float64) if text and text.strip() else np.zeros(0)


def _node_matrix(node):
    matrix = np.identity(4)
    for child in node:
        tag = _strip(child.tag)
        if tag == "matrix":
            matrix = matrix @ _floats(child.text).reshape(4, 4)
        elif tag == "translate":
            t = np.identity(4)
            t[:3, 3] = _floats(child.text)[:3]
            matrix = matrix @ t
        elif tag == "rotate":
            x, y, z, angle = _floats(child.text)[:4]
            r = np.identity(4)
            axis = np.array([x, y, z])
            n = np.linalg.norm(axis)
            if n > 1e-12:
                x, y, z = axis / n
                a = np.radians(angle)
                c, s, t = np.cos(a), np.sin(a), 1 - np.cos(a)
                r[:3, :3] = [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
                             [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
                             [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]
            matrix = matrix @ r
        elif tag == "scale":
            matrix = matrix @ np.diag(list(_floats(child.text)[:3]) + [1.0])
    return matrix


_SIZE = re.compile(r"(-?\d+)$")


def detail_size(name):
    match = _SIZE.search(name)
    return int(match.group(1)) if match else None


class _Document:
    def __init__(self, data):
        root = ElementTree.fromstring(data)
        for element in root.iter():
            element.tag = _strip(element.tag)
        self.root = root
        self.ids = {}
        for element in root.iter():
            identifier = element.get("id")
            if identifier:
                self.ids[identifier] = element
        self.material_names = {}
        for material in root.iter("material"):
            identifier = material.get("id", "")
            name = material.get("name") or (identifier[:-9] if identifier.endswith("-material") else identifier)
            self.material_names[identifier] = name
        up = root.find("asset/up_axis")
        unit = root.find("asset/unit")
        self.meter = float(unit.get("meter", "1")) if unit is not None else 1.0
        self.up = up.text.strip().upper() if up is not None and up.text else "Z_UP"

    def lookup(self, url):
        return self.ids.get(url.lstrip("#"))

    def source(self, url):
        """A <source>'s float_array reshaped by its accessor's stride."""
        source = self.lookup(url)
        if source is None:
            return None
        if source.tag == "vertices":
            return source
        array = source.find("float_array")
        accessor = source.find("technique_common/accessor")
        values = _floats(array.text)
        stride = int(accessor.get("stride", "1")) if accessor is not None else 1
        count = len(values) // stride
        return values[: count * stride].reshape(count, stride)


def _primitive_triangles(document, element):
    """(attribute arrays dict, triangle-corner index table) for a <triangles>/<polylist>/<polygons>."""
    inputs = []
    for item in element.findall("input"):
        inputs.append((item.get("semantic"), item.get("source"), int(item.get("offset", "0")), int(item.get("set", "0"))))
    if not inputs:
        return None
    stride = max(offset for _, _, offset, _ in inputs) + 1
    if element.tag == "polygons":
        polygons = [_floats(p.text).astype(np.int64).reshape(-1, stride) for p in element.findall("p")]
    else:
        p = element.find("p")
        if p is None or not p.text:
            return None
        corners = _floats(p.text).astype(np.int64).reshape(-1, stride)
        if element.tag == "triangles":
            polygons = None
        else:
            counts = _floats(element.find("vcount").text).astype(np.int64)
            starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
            polygons = [corners[s: s + c] for s, c in zip(starts, counts)]
    if polygons is not None:
        fan = []
        for polygon in polygons:
            for k in range(1, len(polygon) - 1):
                fan.extend([polygon[0], polygon[k], polygon[k + 1]])
        corners = np.array(fan, dtype=np.int64).reshape(-1, stride) if fan else np.zeros((0, stride), dtype=np.int64)

    attributes = {}
    for semantic, url, offset, set_index in inputs:
        if semantic == "VERTEX":
            vertices = document.lookup(url)
            for item in vertices.findall("input"):
                data = document.source(item.get("source"))
                if data is not None:
                    attributes[item.get("semantic")] = (data, offset)
        else:
            data = document.source(url)
            if data is None:
                continue
            key = semantic if semantic != "TEXCOORD" else "TEXCOORD{}".format(set_index)
            if semantic == "COLOR":
                key = "COLOR" if "COLOR" not in attributes else "COLOR{}".format(set_index)
            attributes.setdefault(key, (data, offset))
    return attributes, corners


def _texcoord_sets(attributes):
    keys = sorted((k for k in attributes if k.startswith("TEXCOORD")), key=lambda k: int(k[8:] or 0))
    return keys


def _build_primitive(document, element, matrix, material_name):
    parsed = _primitive_triangles(document, element)
    if parsed is None:
        return None
    attributes, corners = parsed
    if "POSITION" not in attributes or len(corners) == 0:
        return None
    # Corners sharing every index become one vertex.
    used_offsets = sorted({offset for _, offset in attributes.values()})
    keys = corners[:, used_offsets]
    unique, inverse = np.unique(keys, axis=0, return_inverse=True)
    inverse = inverse.reshape(-1)
    column = {offset: i for i, offset in enumerate(used_offsets)}

    def gather(name, width):
        if name not in attributes:
            return None
        data, offset = attributes[name]
        index = unique[:, column[offset]]
        index = np.clip(index, 0, len(data) - 1)
        return data[index, :width] if data.shape[1] >= width else np.pad(data[index], ((0, 0), (0, width - data.shape[1])), constant_values=1.0)

    positions = gather("POSITION", 3)
    normals = gather("NORMAL", 3)
    sets = _texcoord_sets(attributes)
    uv0 = gather(sets[0], 2) if sets else None
    uv1 = gather(sets[1], 2) if len(sets) > 1 else None
    colors = gather("COLOR", 4) if "COLOR" in attributes else None

    homogeneous = np.c_[positions, np.ones(len(positions))]
    positions = (homogeneous @ matrix.T)[:, :3]
    if normals is not None:
        normal_matrix = np.linalg.inv(matrix[:3, :3]).T
        normals = normals @ normal_matrix.T
        lengths = np.linalg.norm(normals, axis=1, keepdims=True)
        normals = np.where(lengths > 1e-12, normals / np.maximum(lengths, 1e-12), np.array([0.0, 0.0, 1.0]))
    indices = inverse.astype(np.uint32)
    if np.linalg.det(matrix[:3, :3]) < 0:
        indices = indices.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1)
    return Primitive(material_name, positions, normals, uv0, uv1, colors, indices)


def load_shape(data):
    document = _Document(data)
    scene = None
    instance = document.root.find("scene/instance_visual_scene")
    if instance is not None:
        scene = document.lookup(instance.get("url"))
    if scene is None:
        scene = document.root.find("library_visual_scenes/visual_scene")
    shape = Shape()
    if scene is None:
        return shape

    root_matrix = np.identity(4)
    root_matrix[:3, :3] *= document.meter
    if document.up == "Y_UP":
        root_matrix = root_matrix @ np.array([[1, 0, 0, 0], [0, 0, -1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=np.float64)
    elif document.up == "X_UP":
        root_matrix = root_matrix @ np.array([[0, -1, 0, 0], [1, 0, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]], dtype=np.float64)

    # Every node holding geometry: (name, matrix, instance_geometry element, inside a start node?).
    meshes = []
    base_found = [False]

    def walk(node, matrix, in_start, parent_name=""):
        matrix = matrix @ _node_matrix(node)
        # 3ds Max writes a mesh's pivot offset as an unnamed child node: it keeps the named parent's name.
        name = node.get("name") or node.get("id") or parent_name
        lower = name.lower()
        if lower.startswith("base") and any((c.get("name") or c.get("id") or "").lower().startswith("start") for c in node.findall("node")):
            base_found[0] = True
        starts_here = in_start or lower.startswith("start")
        for geometry in node.findall("instance_geometry"):
            meshes.append((name, matrix, geometry, starts_here))
        for child in node.findall("instance_node"):
            target = document.lookup(child.get("url", ""))
            if target is not None:
                walk(target, matrix, starts_here, name)
        for child in node.findall("node"):
            walk(child, matrix, starts_here, name)

    for node in scene.findall("node"):
        walk(node, root_matrix, False)

    def is_collision_name(name):
        lower = name.lower()
        return lower.startswith(("colmesh", "collision", "col-", "col_"))

    def is_los(name):
        return name.lower().startswith("los")

    visible_size = None
    if base_found[0]:
        sizes = [detail_size(name) for name, _, _, in_start in meshes if in_start and not is_los(name)]
        sizes = [s for s in sizes if s is not None and s >= 0]
        visible_size = max(sizes) if sizes else None
    shape.detail_size = visible_size

    for name, matrix, geometry_instance, in_start in meshes:
        geometry = document.lookup(geometry_instance.get("url", ""))
        if geometry is None or geometry.find("mesh") is None:
            continue
        size = detail_size(name)
        if is_los(name):
            continue
        # Under a base node a negative size is a collision level; a lone shape names its collision.
        collision = (size is not None and size < 0 and in_start) if base_found[0] else is_collision_name(name)
        if not collision:
            if base_found[0]:
                if not in_start:
                    continue
                if size is not None and size < 0:
                    continue
                if visible_size is not None and size is not None and size != visible_size:
                    continue
        bindings = {}
        for bound in geometry_instance.iter("instance_material"):
            target = bound.get("target", "").lstrip("#")
            bindings[bound.get("symbol")] = document.material_names.get(target, target[:-9] if target.endswith("-material") else target)
        for element in geometry.find("mesh"):
            if element.tag not in ("triangles", "polylist", "polygons"):
                continue
            symbol = element.get("material")
            material = bindings.get(symbol, document.material_names.get(symbol or "", symbol or ""))
            primitive = _build_primitive(document, element, matrix, material)
            if primitive is None:
                continue
            if collision:
                shape.collision.append((primitive.positions, primitive.indices, material))
            else:
                shape.primitives.append(primitive)
                shape.materials.add(material)
    return shape
