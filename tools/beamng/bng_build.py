#!/usr/bin/env python3
"""Converts a BeamNG.drive level into one MiniEngine glTF: every static shape at its highest detail,
the terrain, the collision BeamNG's own physics uses, and its water blocks.

    python tools/beamng/bng_build.py gridmap_v2 [--out DIR] [--game DIR] [--cell 256] [--install ROOT]

Reads the game's zips directly (bng_vfs). Writes DIR/<model>/<model>.gltf, .bin and textures/*.png, where
<model> is beamng_<level> and DIR defaults to C:/Project/BeamNG MAP/out. --install ROOT then puts the model
in ROOT/assets/models/<model>/ (hard links where it can), replacing what is there, with an asset sidecar
whose uuid comes from the level's name, so a rebuilt model keeps the uuid scenes refer to. The model keeps BeamNG's own
coordinates (metres, X east, Y north, Z up) under a root node that turns Z up into glTF's Y up, so a
BeamNG position (x, y, z) is (x, z, -y) in the engine and north is -Z.

What goes in:
- TSStatic shapes (level and prefabs): the highest detail level's meshes, merged per CELL x CELL metres
  and material. Collision follows the object's collisionType: "Collision Mesh" (the default) the shape's
  Colmesh meshes, "Visible Mesh" / "Visible Mesh Final" the drawn meshes, "None" nothing.
- The TerrainBlock (bng_terrain): one material per terrain layer, colliding everywhere.
- WaterBlocks: their top face as MINIENGINE_water.
Every collision triangle takes the ground model of its material (Material.groundType, a terrain layer's
groundmodelName; ASPHALT without one), written as MINIENGINE_collision with the grip relative to dry
asphalt (BeamNG's staticFrictionCoefficient / ASPHALT's).
Left out: DecalRoads (road markings and AI paths drawn onto other surfaces), GroundCover grass, rivers,
lights, the sky. Materials use their first stage only, without detail maps.
"""

import argparse
import hashlib
import json
import multiprocessing
import os
import re
import struct
import sys
import shutil
import time
import uuid

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bng_dae  # noqa: E402
import bng_materials  # noqa: E402
import bng_scene  # noqa: E402
import bng_terrain  # noqa: E402
import bng_vfs  # noqa: E402

DEFAULT_OUT = r"C:\Project\BeamNG MAP\out"
GLTF_FLOAT, GLTF_UINT = 5126, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963

# How a loose surface grips beyond its friction, as tools/render_scenes/make_car_test_track.py's GRIP
# (PhysicsWorld's SurfaceGrip: sliding share of the peak, added rolling resistance), by BeamNG ground model.
GRIP_EXTRAS = {
    "GRASS": {"slidingShare": 0.9, "rollingResistance": 0.06},
    "DIRT": {"slidingShare": 0.96, "rollingResistance": 0.037},
    "DIRT_DUSTY": {"slidingShare": 0.96, "rollingResistance": 0.037},
    "GRAVEL": {"slidingShare": 0.92, "rollingResistance": 0.012},
    "MUD": {"slidingShare": 0.82, "rollingResistance": 0.09},
    "SAND": {"slidingShare": 0.9, "rollingResistance": 0.08},
    "SNOW": {"slidingShare": 0.75, "rollingResistance": 0.013},
    "ICE": {"slidingShare": 0.7},
    "ASPHALT_WET": {"wetSpeedFalloff": 0.0173, "slidingShare": 0.86},
}
TERRAIN_ROUGHNESS = 0.85


# ----------------------------------------------------------------------------- shapes (parallel)

_vfs = None


def _init_worker(game, level):
    global _vfs
    _vfs = bng_vfs.Vfs(game, [level])


def _load_shape(path):
    try:
        return path, bng_dae.load_shape(_vfs.read(path)), None
    except Exception as error:  # a broken shape is reported and left out, not fatal
        return path, None, "{}: {}".format(type(error).__name__, error)


# ----------------------------------------------------------------------------- textures (parallel)

def _srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _open_image(key):
    image = Image.open(__import__("io").BytesIO(_vfs.read_entry(key)))
    image.load()
    return image


def _make_texture(job):
    """Writes one PNG. job = (out_path, kind, sources...)."""
    out_path, kind = job[0], job[1]
    if os.path.exists(out_path):
        return out_path, None
    try:
        if kind == "color":
            image = _open_image(job[2]).convert("RGBA")
            if job[3]:  # an opacity map into alpha
                opacity = _open_image(job[3]).convert("L").resize(image.size)
                image.putalpha(opacity)
            image.save(out_path, compress_level=1)
        elif kind == "normal":
            image = _open_image(job[2])
            rgb = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
            # Two-channel (BC5) normal maps leave blue empty: rebuild it from the unit length.
            if image.mode in ("LA", "L") or rgb[..., 2].mean() < 0.1:
                x, y = rgb[..., 0] * 2 - 1, rgb[..., 1] * 2 - 1
                rgb[..., 2] = (np.sqrt(np.clip(1 - x * x - y * y, 0, 1)) + 1) / 2
            Image.fromarray(np.clip(rgb * 255 + 0.5, 0, 255).astype(np.uint8)).save(out_path, compress_level=1)
        elif kind == "metal_rough":
            rough_key, metal_key = job[2], job[3]
            images = [(_open_image(k).convert("L") if k else None) for k in (rough_key, metal_key)]
            size = max((im.size for im in images if im is not None), key=lambda s: s[0] * s[1])
            channels = [np.full(size[::-1], 255, np.uint8)]
            for im in images:
                channels.append(np.asarray(im.resize(size)) if im is not None else np.full(size[::-1], 255, np.uint8))
            Image.fromarray(np.stack(channels, -1)).save(out_path, compress_level=1)
        elif kind == "gray":
            _open_image(job[2]).convert("L").convert("RGB").save(out_path, compress_level=1)
        elif kind == "terrain_color":
            # A terrain layer's detail map is grey around 0.5 and modulates the block's base colour.
            base = np.asarray(_open_image(job[2]).convert("RGB"), dtype=np.float32).mean((0, 1)) / 255.0
            detail = np.asarray(_open_image(job[3]).convert("RGB"), dtype=np.float32) / 255.0
            strength = job[4]
            color = base * (1 + (detail * 2 - 1) * strength * 2)
            Image.fromarray(np.clip(color * 255 + 0.5, 0, 255).astype(np.uint8)).save(out_path, compress_level=1)
        return out_path, None
    except Exception as error:
        return out_path, "{}: {}".format(type(error).__name__, error)


# ----------------------------------------------------------------------------- glTF writer

class GltfWriter:
    def __init__(self):
        self.blob = bytearray()
        self.buffer_views = []
        self.accessors = []
        self.meshes = []
        self.nodes = []
        self.materials = []
        self.textures = []
        self.images = []
        self.image_index = {}
        self.extensions = set()

    def _view(self, data, target):
        while len(self.blob) % 4:
            self.blob.append(0)
        self.buffer_views.append({"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data), "target": target})
        self.blob += data
        return len(self.buffer_views) - 1

    def accessor(self, array, kind, target, with_bounds=False):
        if kind == "SCALAR":
            array = np.ascontiguousarray(array, dtype=np.uint32)
            component, count = GLTF_UINT, len(array)
        else:
            array = np.ascontiguousarray(array, dtype=np.float32)
            component, count = GLTF_FLOAT, len(array)
        accessor = {"bufferView": self._view(array.tobytes(), target), "componentType": component, "count": count, "type": kind}
        if with_bounds:
            accessor["min"] = array.min(0).tolist()
            accessor["max"] = array.max(0).tolist()
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def image(self, uri):
        if uri not in self.image_index:
            self.images.append({"uri": uri})
            self.textures.append({"source": len(self.images) - 1, "sampler": 0})
            self.image_index[uri] = len(self.textures) - 1
        return self.image_index[uri]

    def mesh(self, name, attributes, indices, material):
        primitive = {"attributes": {}, "indices": self.accessor(indices, "SCALAR", ELEMENT_ARRAY_BUFFER), "mode": 4}
        for semantic, array in attributes.items():
            kind = {2: "VEC2", 3: "VEC3", 4: "VEC4"}[array.shape[1]]
            primitive["attributes"][semantic] = self.accessor(array, kind, ARRAY_BUFFER, semantic == "POSITION")
        if material is not None:
            primitive["material"] = material
        self.meshes.append({"name": name, "primitives": [primitive]})
        return len(self.meshes) - 1

    def write(self, path, generator, root_children):
        bin_name = os.path.splitext(os.path.basename(path))[0] + ".bin"
        root = {"name": "beamng_z_up", "rotation": [-0.70710678, 0.0, 0.0, 0.70710678], "children": root_children}
        self.nodes.append(root)
        document = {
            "asset": {"version": "2.0", "generator": generator},
            "scene": 0,
            "scenes": [{"nodes": [len(self.nodes) - 1]}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "accessors": self.accessors,
            "bufferViews": self.buffer_views,
            "buffers": [{"uri": bin_name, "byteLength": len(self.blob)}],
        }
        if self.images:
            document["images"] = self.images
            document["textures"] = self.textures
            document["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
        if self.extensions:
            document["extensionsUsed"] = sorted(self.extensions)
        with open(os.path.join(os.path.dirname(path), bin_name), "wb") as file:
            file.write(self.blob)
        with open(path, "w", newline="\n") as file:
            json.dump(document, file, indent=1)


# ----------------------------------------------------------------------------- materials

def _safe_name(key):
    stem = os.path.splitext(os.path.basename(key))[0]
    stem = re.sub(r"[^A-Za-z0-9_.-]", "_", stem)
    return "{}_{}".format(stem, hashlib.md5(key.encode()).hexdigest()[:6])


class MaterialBuilder:
    """Turns BeamNG Materials into glTF materials, queueing the PNGs they need."""

    def __init__(self, vfs, library, writer, texture_dir):
        self.vfs = vfs
        self.library = library
        self.writer = writer
        self.texture_dir = texture_dir
        self.jobs = {}
        self.index = {}
        self.uses_vertex_color = {}
        self.missing_textures = set()
        self.missing_materials = set()

    def _texture(self, kind, *sources):
        keys = []
        for source in sources:
            if source is None:
                keys.append(None)
                continue
            key = self.vfs.find_texture(source)
            if key is None:
                self.missing_textures.add(source)
            keys.append(key)
        if keys[0] is None and kind != "metal_rough":
            return None
        if kind == "metal_rough" and all(k is None for k in keys):
            return None
        name = _safe_name("|".join(str(k) for k in keys) + kind) + ".png"
        self.jobs[name] = tuple([os.path.join(self.texture_dir, name), kind] + keys)
        return self.writer.image("textures/" + name)

    def _texture_info(self, texture, stage, map_name, extra=None):
        info = {"index": texture}
        if int(stage.get(map_name + "UseUV", 0) or 0) == 1:
            info["texCoord"] = 1
        if extra:
            info.update(extra)
        return info

    def get(self, name):
        key = (name or "").lower()
        if key in self.index:
            return self.index[key]
        material = self.library.get(name)
        gltf = {"name": name or "default", "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 0.7}}
        vertex_color = False
        if material is None:
            self.missing_materials.add(name)
            gltf["pbrMetallicRoughness"]["baseColorFactor"] = [0.5, 0.5, 0.5, 1.0]
        else:
            stage = (material.get("Stages") or [{}])[0] or {}
            pbr = gltf["pbrMetallicRoughness"]
            vertex_color = bool(stage.get("vertColor", False))
            v15 = float(material.get("version", 0) or 0) >= 1.5 or "baseColorMap" in stage
            color_map = stage.get("baseColorMap") or stage.get("colorMap") or stage.get("diffuseMap")
            color_factor = stage.get("baseColorFactor") or stage.get("diffuseColor") or [1, 1, 1, 1]
            color_factor = [float(c) for c in (list(color_factor) + [1, 1, 1, 1])[:4]]
            # BeamNG's colour factors are picked in sRGB; glTF's are linear.
            pbr["baseColorFactor"] = list(_srgb_to_linear(color_factor[:3])) + [color_factor[3]]
            opacity = stage.get("opacityMap")
            if color_map:
                texture = self._texture("color", color_map, opacity)
                if texture is not None:
                    pbr["baseColorTexture"] = self._texture_info(texture, stage, "baseColorMap" if v15 else "diffuseMap")
            roughness = stage.get("roughnessFactor")
            if v15:
                metallic = stage.get("metallicFactor")
                rough_map, metal_map = stage.get("roughnessMap"), stage.get("metallicMap")
                if rough_map or metal_map:
                    texture = self._texture("metal_rough", rough_map, metal_map)
                    if texture is not None:
                        pbr["metallicRoughnessTexture"] = self._texture_info(texture, stage, "roughnessMap")
                pbr["roughnessFactor"] = float(roughness) if roughness is not None else (1.0 if rough_map else 0.7)
                pbr["metallicFactor"] = float(metallic) if metallic is not None else (1.0 if metal_map else 0.0)
                ao_map = stage.get("ambientOcclusionMap")
                if ao_map:
                    texture = self._texture("gray", ao_map)
                    if texture is not None:
                        gltf["occlusionTexture"] = self._texture_info(texture, stage, "ambientOcclusionMap")
            else:
                if roughness is not None:
                    pbr["roughnessFactor"] = float(roughness)
                elif stage.get("specularPower") is not None:
                    power = float(stage["specularPower"])
                    pbr["roughnessFactor"] = float(np.sqrt(2.0 / (power + 2.0)))
            normal_map = stage.get("normalMap")
            if normal_map:
                texture = self._texture("normal", normal_map)
                if texture is not None:
                    gltf["normalTexture"] = self._texture_info(texture, stage, "normalMap")
            emissive = stage.get("emissiveFactor")
            if stage.get("glow") or stage.get("emissive"):
                gltf["emissiveFactor"] = list(_srgb_to_linear(color_factor[:3]))
            elif emissive:
                gltf["emissiveFactor"] = [float(e) for e in list(emissive)[:3]]
            if material.get("alphaTest"):
                gltf["alphaMode"] = "MASK"
                gltf["alphaCutoff"] = max(float(material.get("alphaRef", 0) or 0), 1.0) / 255.0
            elif material.get("translucent"):
                gltf["alphaMode"] = "BLEND"
            if material.get("doubleSided"):
                gltf["doubleSided"] = True
        self.writer.materials.append(gltf)
        self.index[key] = len(self.writer.materials) - 1
        self.uses_vertex_color[self.index[key]] = vertex_color
        return self.index[key]

    def terrain(self, layer_name, terrain_material):
        key = "terrain:" + layer_name
        if key in self.index:
            return self.index[key]
        gltf = {"name": "terrain_" + layer_name, "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": TERRAIN_ROUGHNESS}}
        if terrain_material is not None:
            size = float(terrain_material.get("detailSize", 2) or 2)
            transform = {"extensions": {"KHR_texture_transform": {"scale": [1.0 / size, 1.0 / size]}}}
            base = terrain_material.get("baseColorBaseTex")
            detail = terrain_material.get("baseColorDetailTex")
            strength = float((terrain_material.get("baseColorDetailStrength") or [0.25])[0])
            if base and detail:
                base_key, detail_key = self.vfs.find_texture(base), self.vfs.find_texture(detail)
                if base_key and detail_key:
                    name = _safe_name(base_key + detail_key + "terrain") + ".png"
                    self.jobs[name] = (os.path.join(self.texture_dir, name), "terrain_color", base_key, detail_key, strength)
                    gltf["pbrMetallicRoughness"]["baseColorTexture"] = dict(index=self.writer.image("textures/" + name), **transform)
                    self.writer.extensions.add("KHR_texture_transform")
            normal = terrain_material.get("normalDetailTex")
            if normal:
                texture = self._texture("normal", normal)
                if texture is not None:
                    gltf["normalTexture"] = dict(index=texture, scale=float((terrain_material.get("normalDetailStrength") or [1])[0]), **transform)
                    self.writer.extensions.add("KHR_texture_transform")
        self.writer.materials.append(gltf)
        self.index[key] = len(self.writer.materials) - 1
        self.uses_vertex_color[self.index[key]] = False
        return self.index[key]


# ----------------------------------------------------------------------------- the build

class Batches:
    """Drawn triangles per (cell, material) and collision triangles per ground model, in BeamNG space."""

    def __init__(self, cell):
        self.cell = cell
        self.draw = {}
        self.collision = {}

    def cell_of(self, x, y):
        return int(np.floor(x / self.cell)), int(np.floor(y / self.cell))

    def add_draw(self, cell, material, positions, normals, uv0, uv1, colors, indices):
        self.draw.setdefault((cell, material), []).append((positions, normals, uv0, uv1, colors, indices))

    def add_collision(self, surface, positions, indices):
        self.collision.setdefault(surface, []).append((positions, indices))


def _transform(matrix, positions, normals):
    positions = positions @ matrix[:3, :3].T + matrix[:3, 3]
    if normals is not None:
        normals = normals @ np.linalg.inv(matrix[:3, :3])
        normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-12)
    return positions, normals


def _smooth_normals(positions, indices):
    triangles = indices.reshape(-1, 3)
    a, b, c = positions[triangles[:, 0]], positions[triangles[:, 1]], positions[triangles[:, 2]]
    face = np.cross(b - a, c - a)
    normals = np.zeros_like(positions)
    for k in range(3):
        np.add.at(normals, triangles[:, k], face)
    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
    return np.where(lengths > 1e-12, normals / np.maximum(lengths, 1e-12), [0.0, 0.0, 1.0])


def _ground_type(material):
    if material is None:
        return None
    return material.get("groundType") or material.get("groundtype")


def build(args):
    started = time.time()
    level_name = args.level
    model = "beamng_" + level_name
    out_dir = os.path.join(args.out, model)
    texture_dir = os.path.join(out_dir, "textures")
    os.makedirs(texture_dir, exist_ok=True)

    vfs = bng_vfs.Vfs(args.game, [level_name])
    level = bng_scene.Level(vfs, level_name)
    library = bng_materials.MaterialLibrary(vfs, level_name)
    grounds = bng_materials.GroundModels(vfs)
    print("{} objects ({} prefabs missing), {} materials".format(len(level.objects), len(level.missing_prefabs), len(library.by_name)))

    statics = [o for o in level.of_class("TSStatic") if o.get("shapeName")]
    shape_paths = sorted({o["shapeName"] for o in statics})
    workers = os.cpu_count() or 1
    with multiprocessing.Pool(workers, _init_worker, (args.game, level_name)) as pool:
        shapes = {}
        for path, shape, error in pool.imap_unordered(_load_shape, shape_paths):
            if error:
                print("  shape {} failed: {}".format(path, error))
            shapes[path] = shape
        print("{} shapes read in {:.1f} s".format(len(shapes), time.time() - started))

        writer = GltfWriter()
        materials = MaterialBuilder(vfs, library, writer, texture_dir)
        batches = Batches(args.cell)

        def surface_of(material_name):
            return grounds.name_of(_ground_type(library.get(material_name)) or "ASPHALT")

        no_collision = 0
        for obj in statics:
            shape = shapes.get(obj["shapeName"])
            if shape is None:
                continue
            world = obj["world"]
            mirrored = np.linalg.det(world[:3, :3]) < 0
            cell = batches.cell_of(world[0, 3], world[1, 3])
            for primitive in shape.primitives:
                positions, normals = _transform(world, primitive.positions, primitive.normals)
                indices = primitive.indices
                if mirrored:
                    indices = indices.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1)
                material = materials.get(primitive.material)
                batches.add_draw(cell, material, positions, normals, primitive.uv0, primitive.uv1, primitive.colors, indices)
            kind = str(obj.get("collisionType", "Collision Mesh")).strip().lower()
            if kind == "none":
                continue
            if kind.startswith("visible mesh"):
                sources = [(p.positions, p.indices, p.material) for p in shape.primitives]
            else:
                sources = [(positions, indices, material) for positions, indices, material in shape.collision]
            if not sources:
                no_collision += 1
            for positions, indices, material in sources:
                positions, _ = _transform(world, positions, None)
                if mirrored:
                    indices = indices.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1)
                batches.add_collision(surface_of(material), positions, indices)
        print("{} statics placed ({} without collision)".format(len(statics), no_collision))

        # The terrain.
        for block in level.of_class("TerrainBlock"):
            path = block.get("terrainFile")
            if not path or not vfs.exists(path):
                print("  terrain {} not found".format(path))
                continue
            terrain = bng_terrain.Terrain(vfs.read(path), bng_scene._floats(block.get("position", [0, 0, 0]), 3),
                                          float(block.get("maxHeight", 2048)), float(block.get("squareSize", 1)))
            layer_materials = [library.terrain.get(name) for name in terrain.material_names]
            chunks = terrain.build_mesh()
            for i0, j0, layer, positions, normals, indices in chunks:
                if layer >= len(terrain.material_names):
                    continue
                name = terrain.material_names[layer]
                terrain_material = layer_materials[layer]
                material = materials.terrain(name, terrain_material)
                uv = np.stack([positions[:, 0], -positions[:, 1]], axis=1)
                cell = batches.cell_of(positions[0, 0], positions[0, 1])
                batches.add_draw(cell, material, positions, normals, uv, None, None, indices)
                ground = (terrain_material or {}).get("groundmodelName") or (terrain_material or {}).get("annotation") or "ASPHALT"
                batches.add_collision(grounds.name_of(ground), positions, indices)
            print("terrain {}: {} chunks, {} triangles".format(path, len(chunks), sum(len(c[5]) // 3 for c in chunks)))

        # Textures.
        jobs = list(materials.jobs.values())
        failed = [(path, error) for path, error in pool.imap_unordered(_make_texture, jobs) if error]
        for path, error in failed:
            print("  texture {} failed: {}".format(os.path.basename(path), error))
        print("{} textures in {:.1f} s".format(len(jobs), time.time() - started))

    # Meshes.
    root_children = []
    drawn_triangles = 0
    for (cell, material), parts in sorted(batches.draw.items()):
        uses_color = materials.uses_vertex_color.get(material, False)
        has_uv1 = any(p[3] is not None for p in parts)
        positions, normals, uv0, uv1, colors, indices = [], [], [], [], [], []
        base = 0
        for p_positions, p_normals, p_uv0, p_uv1, p_colors, p_indices in parts:
            count = len(p_positions)
            if p_normals is None:
                p_normals = _smooth_normals(p_positions, p_indices)
            positions.append(p_positions)
            normals.append(p_normals)
            uv0.append(p_uv0 if p_uv0 is not None else np.zeros((count, 2)))
            if has_uv1:
                uv1.append(p_uv1 if p_uv1 is not None else np.zeros((count, 2)))
            if uses_color:
                colors.append(p_colors if p_colors is not None else np.ones((count, 4)))
            indices.append(p_indices.astype(np.int64) + base)
            base += count
        attributes = {"POSITION": np.concatenate(positions), "NORMAL": np.concatenate(normals)}
        texcoord = np.concatenate(uv0)
        is_terrain = writer.materials[material]["name"].startswith("terrain_")
        # COLLADA's V runs up the image, glTF's down (Torque flips V on import too).
        attributes["TEXCOORD_0"] = texcoord if is_terrain else np.c_[texcoord[:, 0], 1.0 - texcoord[:, 1]]
        if has_uv1:
            texcoord = np.concatenate(uv1)
            attributes["TEXCOORD_1"] = np.c_[texcoord[:, 0], 1.0 - texcoord[:, 1]]
        if uses_color:
            attributes["COLOR_0"] = np.clip(np.concatenate(colors), 0.0, 1.0)
        all_indices = np.concatenate(indices)
        drawn_triangles += len(all_indices) // 3
        name = "{}_{}_{}".format(writer.materials[material]["name"], cell[0], cell[1])
        mesh = writer.mesh(name, attributes, all_indices, material)
        writer.nodes.append({"name": name, "mesh": mesh})
        root_children.append(len(writer.nodes) - 1)

    collision_triangles = 0
    for surface, parts in sorted(batches.collision.items()):
        model_ground = grounds.get(surface)
        asphalt = grounds.get("ASPHALT")
        static = float(model_ground.get("staticFrictionCoefficient", 0.98))
        sliding = float(model_ground.get("slidingFrictionCoefficient", 0.7))
        extension = {"surface": surface, "friction": round(static / float(asphalt["staticFrictionCoefficient"]), 4)}
        extension.update(GRIP_EXTRAS.get(surface, {}))
        if "slidingShare" in extension and static > 0:
            extension["slidingShare"] = round(min(1.0, max(extension["slidingShare"], sliding / static)), 4)
        positions = np.concatenate([p for p, _ in parts])
        offsets = np.cumsum([0] + [len(p) for p, _ in parts])[:-1]
        indices = np.concatenate([i.astype(np.int64) + o for (_, i), o in zip(parts, offsets)])
        collision_triangles += len(indices) // 3
        mesh = writer.mesh("collision_" + surface, {"POSITION": positions}, indices, None)
        writer.nodes.append({"name": "collision_" + surface, "mesh": mesh, "extensions": {"MINIENGINE_collision": extension}})
        writer.extensions.add("MINIENGINE_collision")
        root_children.append(len(writer.nodes) - 1)
        print("  collision {:<14} {:>9} triangles  {}".format(surface, len(indices) // 3, extension))

    # Water blocks: the top of the block (Torque's WaterBlock box hangs below its position).
    water_material = None
    for block in level.of_class("WaterBlock"):
        matrix = block["world"]
        corners = np.array([[-0.5, -0.5, 0], [0.5, -0.5, 0], [0.5, 0.5, 0], [-0.5, 0.5, 0]])
        scale = np.diag(bng_scene._floats(block.get("scale", [1, 1, 1]), 3))
        unscaled = matrix[:3, :3] @ np.linalg.inv(scale)
        positions = (corners @ scale) @ unscaled.T + matrix[:3, 3]
        if water_material is None:
            writer.materials.append({
                "name": "beamng_water", "doubleSided": True,
                "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.05},
                "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1.0}, "KHR_materials_ior": {"ior": 1.333},
                               "KHR_materials_volume": {"thicknessFactor": 2.0, "attenuationDistance": 4.0, "attenuationColor": [0.25, 0.4, 0.35]}}})
            water_material = len(writer.materials) - 1
            writer.extensions.update(["KHR_materials_transmission", "KHR_materials_ior", "KHR_materials_volume", "MINIENGINE_water"])
        normals = np.tile([0.0, 0.0, 1.0], (4, 1))
        mesh = writer.mesh("water_" + block.get("name", "block"), {"POSITION": positions, "NORMAL": normals}, np.array([0, 1, 2, 0, 2, 3]), water_material)
        writer.nodes.append({"name": "water_" + block.get("name", "block"), "mesh": mesh, "extensions": {"MINIENGINE_water": {}}})
        root_children.append(len(writer.nodes) - 1)

    gltf_path = os.path.join(out_dir, model + ".gltf")
    writer.write(gltf_path, "tools/beamng/bng_build.py ({})".format(level_name), root_children)
    report = {
        "level": level_name,
        "drawn_triangles": drawn_triangles,
        "collision_triangles": collision_triangles,
        "meshes": len(writer.meshes),
        "materials": len(writer.materials),
        "textures": len(materials.jobs),
        "missing_materials": sorted(m for m in materials.missing_materials if m),
        "missing_textures": sorted(materials.missing_textures),
        "spawn_points": {o.get("name"): {"position": bng_scene._floats(o["position"], 3), "rotationMatrix": o["world"][:3, :3].reshape(-1).round(6).tolist()}
                         for o in level.of_class("SpawnSphere")},
    }
    with open(os.path.join(out_dir, "build_report.json"), "w") as file:
        json.dump(report, file, indent=1)
    print("{}: {} drawn triangles, {} collision triangles, {} meshes, {:.0f} MB, {:.1f} s".format(
        gltf_path, drawn_triangles, collision_triangles, len(writer.meshes), len(writer.blob) / 1e6, time.time() - started))
    if report["missing_materials"]:
        print("missing materials:", report["missing_materials"])
    if report["missing_textures"]:
        print("missing textures:", report["missing_textures"])


def install(out_dir, model, root):
    target = os.path.join(root, "assets", "models", model)
    if os.path.isdir(target):
        shutil.rmtree(target)
    for folder, _, files in os.walk(out_dir):
        destination = os.path.join(target, os.path.relpath(folder, out_dir))
        os.makedirs(destination, exist_ok=True)
        for name in files:
            if name == "build_report.json":
                continue
            source = os.path.join(folder, name)
            try:
                os.link(source, os.path.join(destination, name))
            except OSError:
                shutil.copy2(source, os.path.join(destination, name))
    asset_uuid = model_uuid(model)
    with open(os.path.join(target, model + ".gltf.miniengine_asset.yaml"), "w", newline="\n") as file:
        file.write("asset:\n  uuid: {}\n  file: {}.gltf".format(asset_uuid, model))
    print("installed {} (uuid {})".format(target, asset_uuid))


def model_uuid(model):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "miniengine:beamng:" + model))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("level")
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--game", default=bng_vfs.DEFAULT_GAME)
    parser.add_argument("--cell", type=float, default=256.0)
    parser.add_argument("--install", metavar="ROOT", help="a MiniEngine checkout to install the model into")
    args = parser.parse_args()
    build(args)
    if args.install:
        install(os.path.join(args.out, "beamng_" + args.level), "beamng_" + args.level, args.install)


if __name__ == "__main__":
    main()
