"""A BeamNG TerrainBlock (.ter) as triangles: heights, the layer (terrain material) of every grid square,
and a mesh that keeps every sample where the ground is not one flat, single-material plane.

The .ter file (version 9, theTerrain.terrain.json describes it): a version byte, the size n (u32), n*n
u16 heights, n*n u8 layer indices (255 = hole), then the material names (u32 count, each a u8 length +
characters). Sample (i, j) lies at the block's position + (i, j) * squareSize, its height position.z +
h / 65535 * maxHeight. A grid square takes the layer of its lower-left sample; Torque splits squares
along alternating diagonals (a checkerboard).

The mesh is built per tile of TILE x TILE squares. A tile whose samples all lie on one plane and whose
squares share one layer is drawn as a fan around its centre; every other tile keeps all its squares.
A tile edge shared by two such plane tiles keeps only its end points, any other edge keeps every sample,
so neighbouring tiles always meet at the same vertices and the surface has no cracks or T-junctions
(an unwelded seam throws a car's wheels, see the rolling road launch work).
"""

import struct

import numpy as np

TILE = 64
PLANE_TOLERANCE = 0.0005  # metres


class Terrain:
    def __init__(self, data, position, max_height, square_size=1.0):
        version = data[0]
        size = struct.unpack_from("<I", data, 1)[0]
        offset = 5
        heights = np.frombuffer(data, dtype="<u2", count=size * size, offset=offset).reshape(size, size)
        offset += 2 * size * size
        layers = np.frombuffer(data, dtype="u1", count=size * size, offset=offset).reshape(size, size)
        offset += size * size
        if version < 9:
            offset += size * size  # an older file's layerTextureMap
        count = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        names = []
        for _ in range(count):
            length = data[offset]
            names.append(data[offset + 1: offset + 1 + length].decode("latin-1"))
            offset += 1 + length
        self.version = version
        self.size = size
        self.square = float(square_size)
        self.origin = np.array(position[:3], dtype=np.float64)
        # [j, i]: row j is y, column i is x.
        self.height = self.origin[2] + heights.astype(np.float64) / 65535.0 * float(max_height)
        self.layers = layers
        self.material_names = names

    def sample_xy(self, i, j):
        return self.origin[0] + np.asarray(i) * self.square, self.origin[1] + np.asarray(j) * self.square

    def normals(self):
        """Per-sample normals from central differences (Z up)."""
        h = self.height
        dx = np.zeros_like(h)
        dy = np.zeros_like(h)
        dx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) / (2 * self.square)
        dx[:, 0] = (h[:, 1] - h[:, 0]) / self.square
        dx[:, -1] = (h[:, -1] - h[:, -2]) / self.square
        dy[1:-1, :] = (h[2:, :] - h[:-2, :]) / (2 * self.square)
        dy[0, :] = (h[1, :] - h[0, :]) / self.square
        dy[-1, :] = (h[-1, :] - h[-2, :]) / self.square
        n = np.stack([-dx, -dy, np.ones_like(h)], axis=-1)
        return n / np.linalg.norm(n, axis=-1, keepdims=True)

    def build_mesh(self):
        """Triangles of the whole block: a list of (tile_i0, tile_j0, layer, positions (k,3), normals
        (k,3), indices) chunks, one per tile and layer. Holes (layer 255) are left out."""
        squares = self.size - 1
        tiles = (squares + TILE - 1) // TILE
        bounds = [(t * TILE, min((t + 1) * TILE, squares)) for t in range(tiles)]
        plane = np.zeros((tiles, tiles), dtype=bool)
        plane_coefficients = {}
        for tj, (j0, j1) in enumerate(bounds):
            for ti, (i0, i1) in enumerate(bounds):
                layer_block = self.layers[j0:j1, i0:i1]
                first = layer_block[0, 0]
                if first == 255 or not np.all(layer_block == first):
                    continue
                h = self.height[j0:j1 + 1, i0:i1 + 1]
                # The plane through three corners; every sample has to lie on it.
                a = h[0, 0]
                b = (h[0, -1] - a) / (i1 - i0)
                c = (h[-1, 0] - a) / (j1 - j0)
                jj, ii = np.mgrid[0:j1 - j0 + 1, 0:i1 - i0 + 1]
                if np.max(np.abs(a + b * ii + c * jj - h)) <= PLANE_TOLERANCE:
                    plane[tj, ti] = True
                    plane_coefficients[(ti, tj)] = (a, b, c)

        sample_normals = self.normals()
        chunks = []
        for tj, (j0, j1) in enumerate(bounds):
            for ti, (i0, i1) in enumerate(bounds):
                if plane[tj, ti]:
                    chunks.append(self._plane_tile(ti, tj, i0, i1, j0, j1, plane, plane_coefficients[(ti, tj)], tiles))
                else:
                    chunks.extend(self._full_tile(ti, tj, i0, i1, j0, j1, sample_normals))
        return [c for c in chunks if c is not None]

    def _point(self, i, j):
        x, y = self.sample_xy(i, j)
        return (x, y, self.height[j, i])

    def _plane_tile(self, ti, tj, i0, i1, j0, j1, plane, coefficients, tiles):
        def coarse(neighbour):
            ni, nj = neighbour
            if not (0 <= ni < tiles and 0 <= nj < tiles):
                return True  # the block's own edge
            return bool(plane[nj, ni])

        # Counter-clockwise from above (x right, y up): bottom, right, top, left.
        loop = []
        edges = [
            ((ti, tj - 1), [(i, j0) for i in range(i0, i1)]),
            ((ti + 1, tj), [(i1, j) for j in range(j0, j1)]),
            ((ti, tj + 1), [(i, j1) for i in range(i1, i0, -1)]),
            ((ti - 1, tj), [(i0, j) for j in range(j1, j0, -1)]),
        ]
        for neighbour, samples in edges:
            loop.extend(samples[:1] if coarse(neighbour) else samples)
        a, b, c = coefficients
        normal = np.array([-b / self.square, -c / self.square, 1.0])
        normal /= np.linalg.norm(normal)
        ci, cj = (i0 + i1) / 2.0, (j0 + j1) / 2.0
        cx, cy = self.sample_xy(ci, cj)
        centre = (cx, cy, a + b * (ci - i0) + c * (cj - j0))
        positions = [centre] + [self._point(i, j) for i, j in loop]
        count = len(loop)
        indices = []
        for k in range(count):
            indices += [0, 1 + k, 1 + (k + 1) % count]
        positions = np.array(positions, dtype=np.float64)
        normals = np.tile(normal, (len(positions), 1))
        return (i0, j0, int(self.layers[j0, i0]), positions, normals, np.array(indices, dtype=np.uint32))

    def _full_tile(self, ti, tj, i0, i1, j0, j1, sample_normals):
        w, h = i1 - i0 + 1, j1 - j0 + 1
        jj, ii = np.mgrid[j0:j1 + 1, i0:i1 + 1]
        x, y = self.sample_xy(ii, jj)
        positions = np.stack([x, y, self.height[j0:j1 + 1, i0:i1 + 1]], axis=-1).reshape(-1, 3)
        normals = sample_normals[j0:j1 + 1, i0:i1 + 1].reshape(-1, 3)
        sj, si = np.mgrid[0:h - 1, 0:w - 1]
        v00 = (sj * w + si).reshape(-1)
        v10 = v00 + 1
        v01 = v00 + w
        v11 = v01 + 1
        even = (((si + i0) + (sj + j0)) % 2 == 0).reshape(-1)
        # Even squares split along (0,0)-(1,1), odd ones along (1,0)-(0,1); both counter-clockwise.
        t1 = np.where(even[:, None], np.stack([v00, v10, v11], 1), np.stack([v00, v10, v01], 1))
        t2 = np.where(even[:, None], np.stack([v00, v11, v01], 1), np.stack([v10, v11, v01], 1))
        layer = self.layers[j0:j1, i0:i1].reshape(-1)
        chunks = []
        for value in np.unique(layer):
            if value == 255:
                continue
            mask = layer == value
            triangles = np.concatenate([t1[mask], t2[mask]]).reshape(-1)
            used, remap = np.unique(triangles, return_inverse=True)
            chunks.append((i0, j0, int(value), positions[used], normals[used], remap.astype(np.uint32)))
        return chunks
