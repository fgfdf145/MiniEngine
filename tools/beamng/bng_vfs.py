"""BeamNG's virtual file system: the game's zips mounted read only, looked up by a game path.

BeamNG mounts every zip under its install's content folders (and gameengine.zip) at the root, so a level
asks for "/levels/gridmap_v2/art/x.dae" or "/assets/materials/y.png" without saying which zip holds it.
Lookups ignore case (the game runs on Windows and its data mixes "x.DDS" and "x.dds"). A ".link" file is a
small JSON pointer to another path ({"path": ...}); open() follows it.
"""

import json
import os
import zipfile

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\BeamNG.drive"


def normalize(path):
    path = path.replace("\\", "/").strip()
    if not path.startswith("/"):
        path = "/" + path
    while "//" in path:
        path = path.replace("//", "/")
    return path.lower()


class Vfs:
    def __init__(self, game=DEFAULT_GAME, levels=()):
        """Mounts the shared content and the named levels' zips (a level's zip is only read when asked
        for: West Coast alone is 3 GB of index)."""
        self.game = game
        self.entries = {}  # normalized path -> (zip index, member name)
        self.zips = []
        archives = [os.path.join(game, "gameengine.zip"), os.path.join(game, "content", "art_shapes.zip")]
        for root, _, files in os.walk(os.path.join(game, "content", "assets")):
            archives += [os.path.join(root, f) for f in sorted(files) if f.lower().endswith(".zip")]
        for level in levels:
            archives.append(os.path.join(game, "content", "levels", level + ".zip"))
        for archive in archives:
            self.mount(archive)

    def mount(self, archive):
        handle = zipfile.ZipFile(archive)
        index = len(self.zips)
        self.zips.append(handle)
        for info in handle.infolist():
            if not info.is_dir():
                self.entries[normalize(info.filename)] = (index, info.filename)

    def resolve(self, path):
        """The normalized path that holds `path`'s data, following .link files, or None."""
        key = normalize(path)
        for _ in range(4):
            if key in self.entries:
                return key
            link = key + ".link"
            if link not in self.entries:
                return None
            target = json.loads(self.read_entry(link).decode("utf-8", "replace"))
            key = normalize(target["path"])
        return None

    def read_entry(self, key):
        index, member = self.entries[key]
        return self.zips[index].read(member)

    def exists(self, path):
        return self.resolve(path) is not None

    def read(self, path):
        key = self.resolve(path)
        if key is None:
            raise FileNotFoundError(path)
        return self.read_entry(key)

    def listdir(self, prefix):
        """Every file path under the folder `prefix` (recursive), normalized."""
        prefix = normalize(prefix).rstrip("/") + "/"
        return [key for key in self.entries if key.startswith(prefix)]

    def find_texture(self, path):
        """A texture path as a material names it, or the same name with another of BeamNG's image
        extensions (materials name x.png where only x.dds ships, and the other way round)."""
        for _ in range(4):
            stem, _ = os.path.splitext(normalize(path))
            candidates = [normalize(path)] + [stem + e for e in (".dds", ".png", ".jpg", ".jpeg", ".tga")]
            for candidate in candidates:
                if candidate in self.entries:
                    return candidate
            # A .link (to a path that may again ship under another extension, as the shared
            # /assets ones do: "x.color.png" links to "x.color.png" stored as "x.color.dds").
            for candidate in candidates:
                if candidate + ".link" in self.entries:
                    path = json.loads(self.read_entry(candidate + ".link").decode("utf-8", "replace"))["path"]
                    break
            else:
                return None
        return None
