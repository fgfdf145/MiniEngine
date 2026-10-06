"""Builds a GTA 3D-era (III / Vice City / San Andreas) suspension bridge in Blender and exports it as
glTF for assets/models/gta_bridge/ (scene: assets/scenes/gta_bridge.yaml).

Run inside Blender (4.2+; written against 5.2): paste into the Text Editor and press Run Script, or
    blender --background --python tools/render_scenes/make_gta_bridge.py -- <out_dir>
It replaces the collections GTA_Bridge and Preview_Env (a little land and water for looking at it in
Blender) and, given an <out_dir> (or EXPORT_DIR below), writes <out_dir>/gta_bridge.gltf with its .bin and
textures/. Import that into the engine (Asset Browser, drag and drop, or --model <out_dir>/gta_bridge.gltf).

The bridge runs along Blender +X (glTF/engine +X too). Both ends are 10 m of flat road at height 0: the west
end at the origin, the east end at (L, 0, 0) (660 m with the defaults), so it is placed by putting its origin
on the end of a road. Everything is low poly with 64-256 px generated textures. The railings are alpha-tested
cards (MASK), which the engine's car physics collides with; nothing else needs a collision model.
Metres, Blender Z up (Y up after export).
"""
import bpy, math, os, sys, json
import numpy as np

# ---------------------------------------------------------------- parameters
P = dict(
    FLAT=10.0,        # flat road at each end (connection pad)
    RAMP=160.0,       # approach ramp length
    SIDE=60.0,        # side span (anchorage -> tower)
    MAIN=200.0,       # main span (tower -> tower)
    H=20.0,           # deck height above ground
    CAMBER=2.0,       # extra rise in middle of suspended part
    ROAD_HALF=7.0,    # 4 lanes x 3.5 m
    WALK_W=2.5,       # sidewalk width
    CURB_H=0.25,
    DECK_D=1.6,       # deck thickness
    TOWER_H=45.0,     # tower height above deck
    LAMP_STEP=25.0,
    HANGER_STEP=10.0,
    PIER_STEP=40.0,
    WATER_Z=-3.0,
    PREVIEW_ENV=True, # simple land + water for preview (separate collection)
)
EXPORT_DIR = ""       # set, or pass after "--" on the command line, to export glTF

FLAT, RAMP, SIDE, MAIN, H = P['FLAT'], P['RAMP'], P['SIDE'], P['MAIN'], P['H']
RH, WW, CH, DD = P['ROAD_HALF'], P['WALK_W'], P['CURB_H'], P['DECK_D']
E = RH + WW                       # deck edge
XR0 = FLAT                        # ramp start
XR1 = XR0 + RAMP                  # west anchorage
XT1 = XR1 + SIDE                  # west tower
XT2 = XT1 + MAIN                  # east tower
XS2 = XT2 + SIDE                  # east anchorage
XE0 = XS2 + RAMP                  # east ramp bottom
L = XE0 + FLAT
GIRDER_W, GIRDER_TOP, GIRDER_BOT = 1.3, 1.05, -DD - 0.4
CY = E + GIRDER_W / 2             # cable / hanger plane
ZTOP = H + P['TOWER_H']

def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)

def deck_z(x):
    if x <= XR0 or x >= L - FLAT:
        return 0.0
    if x < XR1:
        return H * smooth((x - XR0) / RAMP)
    if x > XS2:
        return H * smooth((XE0 - x) / RAMP)
    return H + P['CAMBER'] * math.sin(math.pi * (x - XR1) / (XS2 - XR1)) ** 2

# ---------------------------------------------------------------- textures
rng = np.random.default_rng(1337)

def vnoise(n, cells):
    g = rng.random((cells, cells))
    t = np.arange(n) / n * cells
    i0 = np.floor(t).astype(int); f = t - i0; i1 = (i0 + 1) % cells
    f = f * f * (3 - 2 * f)
    fy, fx = f[:, None], f[None, :]
    a = g[np.ix_(i0, i0)] * (1 - fx) + g[np.ix_(i0, i1)] * fx
    b = g[np.ix_(i1, i0)] * (1 - fx) + g[np.ix_(i1, i1)] * fx
    return a * (1 - fy) + b * fy

def fbm(n, octaves=(4, 8, 16, 32, 64), gain=0.55):
    out = np.zeros((n, n)); w = 1.0; tot = 0
    for c in octaves:
        if c > n: break
        out += vnoise(n, c) * w; tot += w; w *= gain
    return out / tot

def make_image(name, rgba):
    h, w = rgba.shape[:2]
    old = bpy.data.images.get(name)
    if old: bpy.data.images.remove(old)
    img = bpy.data.images.new(name, w, h, alpha=True)
    img.pixels.foreach_set(np.clip(rgba, 0, 1).astype(np.float32).ravel())
    img.pack()
    return img

def tex_asphalt():
    n = 256; ppm = n / (2 * RH)               # pixels per metre across road
    base = 0.20 + 0.10 * (fbm(n) - 0.5) + 0.05 * (rng.random((n, n)) - 0.5)
    col = np.stack([base, base, base * 1.03], -1)
    u = (np.arange(n) + 0.5) / ppm             # metres across
    v = np.arange(n)                           # rows along road
    # tyre wear (darker) in lane centres
    for lc in (1.75, 5.25, 8.75, 12.25):
        for off in (-0.9, 0.9):
            band = np.exp(-((u - (lc + off)) / 0.35) ** 2)
            col *= (1 - 0.18 * band)[None, :, None]
    def paint(mask, rgb):
        wear = 0.75 + 0.25 * fbm(n, (8, 16, 32))
        for k in range(3):
            col[..., k] = np.where(mask, rgb[k] * wear, col[..., k])
    U = np.broadcast_to(u[None, :], (n, n)); V = np.broadcast_to(v[:, None], (n, n))
    white, yellow = (0.85, 0.85, 0.82), (0.85, 0.65, 0.12)
    paint((np.abs(U - 0.4) < 0.08) | (np.abs(U - (2 * RH - 0.4)) < 0.08), white)   # edge lines
    dash = (V % 128) < 54
    paint(((np.abs(U - 3.5) < 0.08) | (np.abs(U - 10.5) < 0.08)) & dash, white)  # lane dashes
    paint((np.abs(U - (RH - 0.16)) < 0.07) | (np.abs(U - (RH + 0.16)) < 0.07), yellow)  # double yellow
    return make_image("TX_Asphalt", np.dstack([col, np.ones((n, n))]))

def tex_concrete():
    n = 128
    b = 0.52 + 0.16 * (fbm(n) - 0.5) + 0.04 * (rng.random((n, n)) - 0.5)
    streak = vnoise(n, 24)[0:1, :].repeat(n, 0) * np.linspace(0.6, 1.0, n)[:, None]
    b *= 1 - 0.12 * np.clip(streak - 0.5, 0, 1) * 2
    seam = np.zeros((n, n)); seam[:, ::64] = 1; seam[::64, :] = 1
    b *= 1 - 0.25 * seam
    col = np.stack([b * 1.02, b, b * 0.94], -1)
    return make_image("TX_Concrete", np.dstack([col, np.ones((n, n))]))

def tex_pavement():
    n = 128
    b = 0.58 + 0.12 * (fbm(n) - 0.5) + 0.05 * (rng.random((n, n)) - 0.5)
    grid = np.zeros((n, n)); grid[:, ::32] = 1; grid[::32, :] = 1
    grid[:, 1::32] = 0.5; grid[1::32, :] = 0.5
    b *= 1 - 0.35 * grid
    col = np.stack([b * 1.03, b * 1.0, b * 0.93], -1)
    return make_image("TX_Pavement", np.dstack([col, np.ones((n, n))]))

def tex_steel():
    n = 128
    nz = fbm(n)
    col = np.zeros((n, n, 3))
    col[..., 0] = 0.58 + 0.10 * (nz - 0.5)
    col[..., 1] = 0.13 + 0.04 * (nz - 0.5)
    col[..., 2] = 0.08 + 0.03 * (nz - 0.5)
    rust = np.clip((fbm(n, (8, 16, 32)) - 0.62) * 5, 0, 1)[..., None]
    col = col * (1 - rust) + np.array([0.30, 0.14, 0.07]) * rust
    plate = np.zeros((n, n)); plate[::32, :] = 1; plate[:, ::64] = 1
    rivet = np.zeros((n, n)); rivet[3::32, 2::6] = 1; rivet[29::32, 2::6] = 1
    col *= (1 - 0.35 * plate)[..., None]
    col *= (1 - 0.25 * rivet)[..., None]
    return make_image("TX_SteelRed", np.dstack([col, np.ones((n, n))]))

def tex_fence():
    n = 64
    a = np.zeros((n, n)); c = np.zeros((n, n, 3)) + np.array([0.42, 0.44, 0.46])
    a[:, 0:3] = 1                      # post
    a[58:64, :] = 1; a[56:58, :] = 1   # top rail
    a[29:32, :] = 1                    # mid rail
    a[0:3, :] = 1                      # bottom rail
    a[:, 3::8] = np.maximum(a[:, 3::8], 1)  # balusters
    c *= (0.85 + 0.3 * (fbm(n, (4, 8, 16)) - 0.5))[..., None]
    c[56:58, :] *= 0.6                 # shading under top rail
    return make_image("TX_Fence", np.dstack([c, a]))

def tex_ground():
    n = 128
    nz = fbm(n)
    col = np.stack([0.22 + 0.10 * nz, 0.32 + 0.12 * nz, 0.12 + 0.05 * nz], -1)
    return make_image("TX_Grass", np.dstack([col, np.ones((n, n))]))

def tex_water():
    n = 128
    nz = fbm(n, (8, 16, 32))
    col = np.stack([0.06 + 0.06 * nz, 0.20 + 0.10 * nz, 0.28 + 0.12 * nz], -1)
    return make_image("TX_Water", np.dstack([col, np.ones((n, n))]))

# ---------------------------------------------------------------- materials
def mat_tex(name, img, rough=0.85, alpha=False):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); out.location = (400, 0)
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled"); bsdf.location = (100, 0)
    tx = nt.nodes.new("ShaderNodeTexImage"); tx.location = (-250, 0)
    tx.image = img; tx.interpolation = 'Linear'
    nt.links.new(tx.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = rough
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = 0.2
    if alpha:
        nt.links.new(tx.outputs["Alpha"], bsdf.inputs["Alpha"])
        if hasattr(m, "surface_render_method"): m.surface_render_method = 'DITHERED'
        if hasattr(m, "blend_method"):
            try: m.blend_method = 'CLIP'
            except Exception: pass
        m.use_backface_culling = False
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    m.diffuse_color = tuple(np.array(img.pixels[:4])) if not alpha else (0.4, 0.4, 0.4, 1)
    return m

def mat_flat(name, rgb, rough=0.6, metal=0.0):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*rgb, 1)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    m.diffuse_color = (*rgb, 1)
    return m

def mat_emit(name, rgb, strength):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*rgb, 1); em.inputs["Strength"].default_value = strength
    nt.links.new(em.outputs[0], out.inputs[0])
    m.diffuse_color = (*rgb, 1)
    return m

# ---------------------------------------------------------------- mesh builder
def newell(pts):
    nx = ny = nz = 0.0
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])
    return (nx, ny, nz)

def box_uv(pts, tile):
    n = newell(pts); ax = max(range(3), key=lambda i: abs(n[i]))
    if ax == 2: return [(p[0] / tile, p[1] / tile) for p in pts]
    if ax == 0: return [(p[1] / tile, p[2] / tile) for p in pts]
    return [(p[0] / tile, p[2] / tile) for p in pts]

class MB:
    def __init__(self):
        self.v, self.f, self.uv, self.mi, self.mats = [], [], [], [], []
    def vert(self, co):
        self.v.append(tuple(co)); return len(self.v) - 1
    def mat(self, m):
        if m not in self.mats: self.mats.append(m)
        return self.mats.index(m)
    def face(self, idx, m, uvs=None, tile=4.0, toward=None):
        """toward: point the normal should face away from (outward test)."""
        pts = [self.v[i] for i in idx]
        if toward is not None:
            n = newell(pts)
            c = [sum(p[k] for p in pts) / len(pts) for k in range(3)]
            d = [c[k] - toward[k] for k in range(3)]
            if sum(n[k] * d[k] for k in range(3)) < 0:
                idx = list(reversed(idx)); pts = list(reversed(pts))
                if uvs: uvs = list(reversed(uvs))
        self.f.append(list(idx))
        self.uv.append(uvs or box_uv(pts, tile))
        self.mi.append(self.mat(m))
    def hexa(self, c8, m, tile=4.0):
        """8 corners: bottom quad (4) then top quad (4), same winding."""
        ids = [self.vert(p) for p in c8]
        ctr = [sum(p[k] for p in c8) / 8 for k in range(3)]
        b, t = ids[:4], ids[4:]
        self.face(b, m, tile=tile, toward=ctr)
        self.face(t, m, tile=tile, toward=ctr)
        for i in range(4):
            j = (i + 1) % 4
            self.face([b[i], b[j], t[j], t[i]], m, tile=tile, toward=ctr)
    def box(self, x0, x1, y0, y1, z0, z1, m, tile=4.0):
        self.hexa([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                   (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], m, tile)
    def build(self, name, coll, parent=None):
        me = bpy.data.meshes.new(name)
        me.from_pydata(self.v, [], self.f)
        uvl = me.uv_layers.new(name="UVMap")
        for poly, uvs, mi in zip(me.polygons, self.uv, self.mi):
            poly.material_index = mi
            for li, uv in zip(poly.loop_indices, uvs):
                uvl.data[li].uv = uv
        for m in self.mats: me.materials.append(m)
        me.update()
        ob = bpy.data.objects.new(name, me)
        coll.objects.link(ob)
        if parent: ob.parent = parent
        return ob

def sweep(mb, xs, zf, pts, mats, uvfun=None, tile=4.0, caps=True):
    """Sweep an open/closed (y,z) polyline along X, offset by zf(x).
    pts listed clockwise (y right, z up) => outward normals."""
    rows = []
    for x in xs:
        z0 = zf(x)
        rows.append([mb.vert((x, y, z0 + z)) for (y, z) in pts])
    for i in range(len(xs) - 1):
        for j in range(len(pts) - 1):
            q = [rows[i][j], rows[i + 1][j], rows[i + 1][j + 1], rows[i][j + 1]]
            uvs = uvfun(i, j) if uvfun and uvfun(i, j) is not None else None
            mb.face(q, mats[j], uvs=uvs, tile=tile)
    if caps:
        for k, sgn in ((0, -1), (len(xs) - 1, 1)):
            x = xs[k]
            ring = rows[k][:-1] if pts[0] == pts[-1] else rows[k]
            mb.face(list(ring), mats[0], tile=tile, toward=(x - sgn * 10, 0, zf(x) - 0.5))
    return rows

def mirror(pts, mats):
    return [(-y, z) for (y, z) in reversed(pts)], list(reversed(mats))

# ---------------------------------------------------------------- build
def build():
    # clean previous bridge
    for cname in ("GTA_Bridge", "Preview_Env"):
        c = bpy.data.collections.get(cname)
        if c:
            for o in list(c.objects): bpy.data.objects.remove(o, do_unlink=True)
            bpy.data.collections.remove(c)
    coll = bpy.data.collections.new("GTA_Bridge")
    bpy.context.scene.collection.children.link(coll)

    M = dict(
        asphalt=mat_tex("M_Asphalt", tex_asphalt(), 0.9),
        concrete=mat_tex("M_Concrete", tex_concrete(), 0.9),
        pave=mat_tex("M_Pavement", tex_pavement(), 0.9),
        steel=mat_tex("M_SteelRed", tex_steel(), 0.55),
        fence=mat_tex("M_Fence", tex_fence(), 0.6, alpha=True),
        pole=mat_flat("M_LampPole", (0.30, 0.31, 0.33), 0.5, 0.6),
        lamp=mat_emit("M_LampGlow", (1.0, 0.78, 0.45), 8.0),
        beacon=mat_emit("M_Beacon", (1.0, 0.05, 0.02), 15.0),
        cable=mat_flat("M_Cable", (0.42, 0.10, 0.06), 0.45, 0.3),
    )

    root = bpy.data.objects.new("GTA_Bridge_Root", None)
    root.empty_display_type = 'PLAIN_AXES'; root.empty_display_size = 5
    coll.objects.link(root)

    # stations along X
    xs = set()
    for a, b, st in ((0, XR0, 5), (XR0, XR1, 5), (XR1, XS2, 10), (XS2, XE0, 5), (XE0, L, 5)):
        x = a
        while x < b - 1e-6: xs.add(round(x, 4)); x += st
    xs.add(L)
    for k in (XT1, XT2): xs.add(k)
    xs = sorted(xs)
    # arc length for road texture
    s = [0.0]
    for i in range(1, len(xs)):
        s.append(s[-1] + math.hypot(xs[i] - xs[i - 1], deck_z(xs[i]) - deck_z(xs[i - 1])))

    # --- deck
    mb = MB()
    sec = [(-E, -DD), (-E, CH), (-RH, CH), (-RH, 0), (RH, 0), (RH, CH), (E, CH), (E, -DD), (-E, -DD)]
    smat = [M['concrete'], M['pave'], M['concrete'], M['asphalt'], M['concrete'], M['pave'], M['concrete'], M['concrete']]
    tile_len = 2 * RH
    def road_uv(i, j):
        if j != 3: return None
        v0, v1 = s[i] / tile_len, s[i + 1] / tile_len
        return [(0, v0), (0, v1), (1, v1), (1, v0)]
    sweep(mb, xs, deck_z, sec, smat, uvfun=road_uv, tile=4.0)
    mb.build("Bridge_Deck", coll, root)

    # --- railings (alpha fence), double sided
    mb = MB()
    for y in (-(E - 0.1), E - 0.1):
        prev = None
        for i, x in enumerate(xs):
            z = deck_z(x) + CH
            a = mb.vert((x, y, z)); b = mb.vert((x, y, z + 1.1))
            if prev:
                pa, pb, ps = prev
                q = [pa, a, b, pb] if y > 0 else [pa, pb, b, a]
                uvq = [(ps / 2, 0), (s[i] / 2, 0), (s[i] / 2, 1), (ps / 2, 1)]
                if y < 0: uvq = [uvq[0], uvq[3], uvq[2], uvq[1]]
                mb.face(q, M['fence'], uvs=uvq)
            prev = (a, b, s[i])
    mb.build("Bridge_Railings", coll, root)

    # --- stiffening girders (suspended part)
    mb = MB()
    gx = [x for x in xs if XR1 <= x <= XS2]
    gsec = [(E, GIRDER_BOT), (E, GIRDER_TOP), (E + GIRDER_W, GIRDER_TOP), (E + GIRDER_W, GIRDER_BOT), (E, GIRDER_BOT)]
    gmat = [M['steel']] * 4
    sweep(mb, gx, deck_z, gsec, gmat, tile=4.0)
    lp, lm = mirror(gsec, gmat)
    sweep(mb, gx, deck_z, lp, lm, tile=4.0)
    mb.build("Bridge_Girders", coll, root)

    # --- towers
    mb = MB()
    def leg_center_y(z):  # legs lean inward
        return 13.8 + (CY - 13.8) * (z / ZTOP)
    def leg_half_y(z):
        return 1.5 + (1.0 - 1.5) * (z / ZTOP)
    for xt in (XT1, XT2):
        zd = deck_z(xt)
        for sg in (-1, 1):
            zb, zt = P['WATER_Z'] - 2, ZTOP
            hxb, hxt = 2.0, 1.5
            cb, ct = sg * 13.8, sg * CY
            hb, ht = 1.5, 1.0
            mb.hexa([(xt - hxb, cb - hb, zb), (xt + hxb, cb - hb, zb), (xt + hxb, cb + hb, zb), (xt - hxb, cb + hb, zb),
                     (xt - hxt, ct - ht, zt), (xt + hxt, ct - ht, zt), (xt + hxt, ct + ht, zt), (xt - hxt, ct + ht, zt)],
                    M['steel'], tile=4.0)
            # saddle cap + beacon
            mb.box(xt - 1.9, xt + 1.9, ct - 1.3, ct + 1.3, zt, zt + 1.2, M['steel'])
            mb.box(xt - 0.3, xt + 0.3, ct - 0.3, ct + 0.3, zt + 1.2, zt + 1.8, M['beacon'])
        # portal beams: (z0, z1)
        for z0, z1 in ((zd - DD - 2.6, zd - DD - 0.05), (zd + 20.0, zd + 22.5), (ZTOP - 6.0, ZTOP - 2.5)):
            zc = (z0 + z1) / 2
            yc = leg_center_y(zc)
            mb.box(xt - 1.4, xt + 1.4, -yc, yc, z0, z1, M['steel'])
        # X bracing panel between upper beams (thin)
        za, zb2 = zd + 22.5, ZTOP - 6.0
        for (y0, z0), (y1, z1) in (((-1, za), (1, zb2)), ((1, za), (-1, zb2))):
            p0 = (y0 * (leg_center_y(z0) - leg_half_y(z0)), z0)
            p1 = (y1 * (leg_center_y(z1) - leg_half_y(z1)), z1)
            dy, dz = p1[0] - p0[0], p1[1] - p0[1]
            ln = math.hypot(dy, dz); ny_, nz_ = -dz / ln * 0.5, dy / ln * 0.5
            c = [(p0[0] - ny_, p0[1] - nz_), (p0[0] + ny_, p0[1] + nz_), (p1[0] + ny_, p1[1] + nz_), (p1[0] - ny_, p1[1] - nz_)]
            mb.hexa([(xt - 0.6, y, z) for (y, z) in c] + [(xt + 0.6, y, z) for (y, z) in c], M['steel'])
    mb.build("Bridge_Towers", coll, root)

    # --- foundations, anchorages, ramp piers (concrete)
    mb = MB()
    for xt in (XT1, XT2):
        mb.box(xt - 7, xt + 7, -18, 18, P['WATER_Z'] - 10, P['WATER_Z'] + 2.5, M['concrete'])
    for xa, d in ((XR1, -1), (XS2, 1)):
        zd = deck_z(xa)
        x0, x1 = (xa - 10, xa) if d < 0 else (xa, xa + 10)
        mb.box(x0, x1, -E - 3, E + 3, -2, zd - DD, M['concrete'])                    # base under deck
        for sg in (-1, 1):
            y0, y1 = sorted((sg * E, sg * (E + 3)))
            mb.box(x0, x1, y0, y1, zd - DD, zd + 3.5, M['concrete'])                 # cable housing
            mb.box(x0 - 0.3, x1 + 0.3, y0 - 0.3, y1 + 0.3, zd + 3.5, zd + 4.0, M['concrete'])
    piers = []
    x = XR0 + P['PIER_STEP'] / 2
    while x < XR1 - 12: piers.append(x); x += P['PIER_STEP']
    piers += [L - p for p in piers]
    for xp in piers:
        zb = deck_z(xp) - DD
        if zb < 1.5: continue
        mb.box(xp - 1.0, xp + 1.0, -E + 0.5, E - 0.5, zb - 1.2, zb, M['concrete'])
        for yc in (-5.0, 5.0):
            mb.box(xp - 0.7, xp + 0.7, yc - 0.7, yc + 0.7, -1.0, zb - 1.2, M['concrete'])
    mb.build("Bridge_Supports", coll, root)

    # --- main cables
    def cable_z(x):
        za = deck_z(XR1) + 2.5
        zt = ZTOP + 0.8
        if x <= XT1:
            t = (x - XR1) / SIDE; return za + (zt - za) * t - 4.0 * 4 * t * (1 - t)
        if x >= XT2:
            t = (XS2 - x) / SIDE; return za + (zt - za) * t - 4.0 * 4 * t * (1 - t)
        zmin = deck_z((XT1 + XT2) / 2) + GIRDER_TOP + 1.8
        t = (x - XT1) / MAIN
        return zmin + (zt - zmin) * (2 * t - 1) ** 2
    mb = MB()
    cx = sorted(set([XR1 - 1] + [XR1 + i * 5 for i in range(int((XS2 - XR1) / 5) + 1)] + [XS2 + 1]))
    r, nseg = 0.45, 6
    for sg in (-1, 1):
        rings = []
        for i, x in enumerate(cx):
            xa_, xb_ = cx[max(i - 1, 0)], cx[min(i + 1, len(cx) - 1)]
            tx, tz = xb_ - xa_, cable_z(min(max(xb_, XR1), XS2)) - cable_z(min(max(xa_, XR1), XS2))
            ln = math.hypot(tx, tz); nx_, nz_ = -tz / ln, tx / ln
            zc = cable_z(min(max(x, XR1), XS2))
            ring = []
            for k in range(nseg):
                a = 2 * math.pi * k / nseg
                ring.append(mb.vert((x + r * math.cos(a) * nx_, sg * CY + r * math.sin(a), zc + r * math.cos(a) * nz_)))
            rings.append((ring, (x, sg * CY, zc)))
        for i in range(len(rings) - 1):
            (ra, ca), (rb, cb) = rings[i], rings[i + 1]
            ctr = [(ca[k] + cb[k]) / 2 for k in range(3)]
            for k in range(nseg):
                kk = (k + 1) % nseg
                mb.face([ra[k], rb[k], rb[kk], ra[kk]], M['cable'], toward=ctr)
    mb.build("Bridge_Cables", coll, root)

    # --- hangers
    mb = MB()
    x = XR1 + P['HANGER_STEP']
    while x < XS2 - 1:
        if min(abs(x - XT1), abs(x - XT2)) > 3:
            z0 = deck_z(x) + GIRDER_TOP; z1 = cable_z(x)
            for sg in (-1, 1):
                mb.box(x - 0.12, x + 0.12, sg * CY - 0.12, sg * CY + 0.12, z0, z1, M['cable'], tile=2)
        x += P['HANGER_STEP']
    mb.build("Bridge_Hangers", coll, root)

    # --- street lamps
    mb = MB()
    x = XR0 + 5
    while x < L - XR0 - 4:
        near_tower = min(abs(x - XT1), abs(x - XT2)) < 5
        if not near_tower:
            zb = deck_z(x) + CH
            for sg in (-1, 1):
                yp = sg * (E - 0.6)
                mb.box(x - 0.15, x + 0.15, yp - 0.15, yp + 0.15, zb, zb + 8.0, M['pole'], tile=2)
                y2 = yp - sg * 2.2
                y0, y1 = sorted((yp, y2))
                mb.box(x - 0.1, x + 0.1, y0, y1, zb + 7.75, zb + 7.95, M['pole'], tile=2)
                y0, y1 = sorted((y2, y2 + sg * 0.9))
                mb.box(x - 0.35, x + 0.35, y0, y1, zb + 7.55, zb + 7.8, M['lamp'], tile=2)
        x += P['LAMP_STEP']
    mb.build("Bridge_Lamps", coll, root)

    # --- connection markers
    for nm, px in (("Bridge_Connect_West", 0.0), ("Bridge_Connect_East", L)):
        e = bpy.data.objects.new(nm, None)
        e.empty_display_type = 'SINGLE_ARROW'; e.empty_display_size = 6
        e.location = (px, 0, 0)
        e.rotation_euler = (0, math.radians(-90 if px == 0 else 90), 0)
        coll.objects.link(e); e.parent = root

    for o in coll.objects:
        if o.type == 'MESH':
            for poly in o.data.polygons: poly.use_smooth = False

    # --- preview environment
    if P['PREVIEW_ENV']:
        env = bpy.data.collections.new("Preview_Env")
        bpy.context.scene.collection.children.link(env)
        grass = mat_tex("M_Grass", tex_ground(), 1.0)
        water = mat_tex("M_Water", tex_water(), 0.15)
        mb = MB()
        mb.box(-150, XR1 + 12, -250, 250, -12, -0.05, grass, tile=16)
        mb.box(XS2 - 12, L + 150, -250, 250, -12, -0.05, grass, tile=16)
        mb.build("Env_Land", env)
        mb = MB()
        ids = [mb.vert(p) for p in ((-150, -250, P['WATER_Z']), (L + 150, -250, P['WATER_Z']),
                                    (L + 150, 250, P['WATER_Z']), (-150, 250, P['WATER_Z']))]
        mb.face(ids, water, tile=20)
        mb.build("Env_Water", env)
    return dict(length=L, towers=(XT1, XT2), deck_height=H, tower_top=ZTOP + 1.8)

def export_gltf(out_dir):
    """Export GTA_Bridge as separate glTF, then fix what Blender's exporter gets wrong for the engine."""
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "gta_bridge.gltf")
    coll = bpy.data.collections["GTA_Bridge"]
    for o in bpy.context.view_layer.objects: o.select_set(False)
    for o in coll.objects: o.select_set(True)
    bpy.context.view_layer.objects.active = bpy.data.objects["GTA_Bridge_Root"]
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLTF_SEPARATE', export_texture_dir="textures",
                              use_selection=True, export_yup=True, export_apply=True,
                              export_lights=False, export_cameras=False, export_extras=False)
    j = json.load(open(path, encoding="utf-8"))
    for m in j["materials"]:
        m.get("extensions", {}).pop("KHR_materials_specular", None)
        if not m.get("extensions"): m.pop("extensions", None)
        # Blender writes BLEND for any texture alpha; the railing is a cut-out (and BLEND would not collide).
        if m["name"] == "M_Fence":
            m["alphaMode"] = "MASK"; m["alphaCutoff"] = 0.5; m["doubleSided"] = True
        else:
            m["doubleSided"] = False  # every mesh is closed with outward normals
        # The engine's emission is luminance (cd/m^2) under exposure: lamps faint by day, bright at night.
        if m["name"] == "M_LampGlow":
            m["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 5000.0
        if m["name"] == "M_Beacon":
            m["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 30000.0
    j["extensionsUsed"] = [e for e in j.get("extensionsUsed", []) if e != "KHR_materials_specular"]
    json.dump(j, open(path, "w", encoding="utf-8"), indent=1)
    return path

info = build()
print("GTA bridge built:", info)
out = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv[1:] else EXPORT_DIR
if out:
    print("Exported:", export_gltf(out))
