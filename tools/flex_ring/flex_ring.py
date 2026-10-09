"""The flexible ring tyre from Python (ctypes over miniengine_flex_ring_c, engine/tyre/flex_ring/flex_ring_c_api.h).

    from flex_ring import FlexRing
    t = FlexRing()                       # the default data (the R34's Semislicks); FlexRing("tyre.tir") for a file
    t.set("inflation_pressure", 2.2)     # basic data in FTire's units (bar)
    t.preprocess()                       # fit the model
    for name, target, achieved in t.targets():
        print(name, target, achieved)
    print(t.steady(speed=60 / 3.6, load=4000, slip_angle=math.radians(3)))   # Fx, Fy, Fz, Mx, My, Mz, Re
    t.report("out/flex_ring", what="all", quick=True)                        # CSV, JSON, log

Stepping it yourself (global axes, z up; the rim rotation's second column is the spin axis):
    t.road_flat()
    t.reset(pos, rot, vel, omega)
    force, moment = t.advance(pos, rot, vel, omega, 0.001)

Run with the DLL's folder as --dll or the environment variable MINIENGINE_FLEX_RING_DLL, or from that folder.
`python flex_ring.py --dll <folder> demo` prints a slip-angle sweep.
"""

import ctypes
import math
import os
import sys

_HEIGHT_FN = ctypes.CFUNCTYPE(ctypes.c_double, ctypes.c_void_p, ctypes.c_double, ctypes.c_double)


class Contact(ctypes.Structure):
    _fields_ = [
        ("blocks", ctypes.c_int),
        ("sliding", ctypes.c_int),
        ("normal_force", ctypes.c_double),
        ("area", ctypes.c_double),
        ("length", ctypes.c_double),
        ("width", ctypes.c_double),
        ("max_pressure", ctypes.c_double),
        ("mean_pressure", ctypes.c_double),
        ("centre", ctypes.c_double * 3),
        ("friction_power", ctypes.c_double),
        ("substeps", ctypes.c_int),
        ("cpu_seconds", ctypes.c_double),
    ]


def _load(dll_folder=None):
    folder = dll_folder or os.environ.get("MINIENGINE_FLEX_RING_DLL") or os.getcwd()
    name = "miniengine_flex_ring_c.dll" if sys.platform == "win32" else "libminiengine_flex_ring_c.so"
    path = os.path.join(folder, name)
    lib = ctypes.CDLL(path)
    d = ctypes.c_double
    dp = ctypes.POINTER(ctypes.c_double)
    vp = ctypes.c_void_p
    sigs = {
        "mefr_version": (ctypes.c_int, []),
        "mefr_last_error": (ctypes.c_char_p, []),
        "mefr_create": (vp, [ctypes.c_char_p]),
        "mefr_destroy": (None, [vp]),
        "mefr_data_count": (ctypes.c_int, []),
        "mefr_data_name": (ctypes.c_char_p, [ctypes.c_int]),
        "mefr_data_unit": (ctypes.c_char_p, [ctypes.c_int]),
        "mefr_data_group": (ctypes.c_char_p, [ctypes.c_int]),
        "mefr_set_data": (ctypes.c_int, [vp, ctypes.c_char_p, d]),
        "mefr_get_data": (ctypes.c_int, [vp, ctypes.c_char_p, dp]),
        "mefr_save_data": (ctypes.c_int, [vp, ctypes.c_char_p]),
        "mefr_preprocess": (ctypes.c_int, [vp, ctypes.c_int]),
        "mefr_target_count": (ctypes.c_int, [vp]),
        "mefr_target": (ctypes.c_int, [vp, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p), dp, dp]),
        "mefr_parameter_count": (ctypes.c_int, [vp]),
        "mefr_parameter": (ctypes.c_int, [vp, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p), dp]),
        "mefr_road_flat": (None, [vp, d, d]),
        "mefr_road_cleat": (None, [vp, d, d, d, d, d, d, d, d, ctypes.c_int, d]),
        "mefr_road_grid": (ctypes.c_int, [vp, d, d, d, d, ctypes.c_int, ctypes.c_int, dp]),
        "mefr_road_callback": (None, [vp, _HEIGHT_FN, vp]),
        "mefr_set_pressure": (None, [vp, d]),
        "mefr_set_tread_depth": (None, [vp, d]),
        "mefr_set_friction_scale": (None, [vp, d]),
        "mefr_reset": (ctypes.c_int, [vp, dp, dp, dp, dp]),
        "mefr_advance": (ctypes.c_int, [vp, dp, dp, dp, dp, d, dp, dp]),
        "mefr_settle": (ctypes.c_int, [vp, dp, dp]),
        "mefr_node_count": (ctypes.c_int, [vp]),
        "mefr_nodes": (None, [vp, dp, dp]),
        "mefr_block_count": (ctypes.c_int, [vp]),
        "mefr_blocks": (ctypes.c_int, [vp, dp, ctypes.c_int]),
        "mefr_contact_stats": (None, [vp, ctypes.POINTER(Contact)]),
        "mefr_steady": (ctypes.c_int, [vp, d, d, d, d, d, ctypes.c_int, dp, dp]),
        "mefr_run_report": (ctypes.c_int, [vp, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]),
    }
    for fn, (res, args) in sigs.items():
        f = getattr(lib, fn)
        f.restype = res
        f.argtypes = args
    return lib


def _arr(values):
    return (ctypes.c_double * len(values))(*values)


class FlexRing:
    def __init__(self, tir_path=None, dll_folder=None):
        self.lib = _load(dll_folder)
        self.handle = self.lib.mefr_create(tir_path.encode() if tir_path else None)
        if not self.handle:
            raise RuntimeError(self.lib.mefr_last_error().decode())
        self._callback = None

    def __del__(self):
        if getattr(self, "handle", None):
            self.lib.mefr_destroy(self.handle)
            self.handle = None

    def _check(self, code):
        if code != 0:
            raise RuntimeError(self.lib.mefr_last_error().decode())

    # Basic data (FTire item names and units).
    def items(self):
        return [(self.lib.mefr_data_name(i).decode(), self.lib.mefr_data_unit(i).decode(), self.lib.mefr_data_group(i).decode())
                for i in range(self.lib.mefr_data_count())]

    def set(self, name, value):
        self._check(self.lib.mefr_set_data(self.handle, name.encode(), float(value)))

    def get(self, name):
        v = ctypes.c_double()
        self._check(self.lib.mefr_get_data(self.handle, name.encode(), ctypes.byref(v)))
        return v.value

    def save(self, path):
        self._check(self.lib.mefr_save_data(self.handle, path.encode()))

    def preprocess(self, fit=True):
        self._check(self.lib.mefr_preprocess(self.handle, 1 if fit else 0))

    def targets(self):
        out = []
        for i in range(self.lib.mefr_target_count(self.handle)):
            name = ctypes.c_char_p()
            target = ctypes.c_double()
            achieved = ctypes.c_double()
            self.lib.mefr_target(self.handle, i, ctypes.byref(name), ctypes.byref(target), ctypes.byref(achieved))
            out.append((name.value.decode(), target.value, achieved.value))
        return out

    def parameters(self):
        out = []
        for i in range(self.lib.mefr_parameter_count(self.handle)):
            name = ctypes.c_char_p()
            unit = ctypes.c_char_p()
            value = ctypes.c_double()
            self.lib.mefr_parameter(self.handle, i, ctypes.byref(name), ctypes.byref(unit), ctypes.byref(value))
            out.append((name.value.decode(), value.value, unit.value.decode()))
        return out

    # Roads.
    def road_flat(self, height=0.0, friction=1.0):
        self.lib.mefr_road_flat(self.handle, height, friction)

    def road_cleat(self, x=0.0, y=0.0, across=(1.0, 0.0), width=0.02, height=0.01, bevel=0.0, top_width=-1.0, semicircular=False, friction=1.0):
        self.lib.mefr_road_cleat(self.handle, x, y, across[0], across[1], width, height, bevel, top_width, 1 if semicircular else 0, friction)

    def road_grid(self, x0, y0, dx, dy, heights_rows):
        ny = len(heights_rows)
        nx = len(heights_rows[0])
        flat = [h for row in heights_rows for h in row]
        self._check(self.lib.mefr_road_grid(self.handle, x0, y0, dx, dy, nx, ny, _arr(flat)))

    def road_function(self, height):
        """height(x, y) -> z; kept alive with the tyre."""
        self._callback = _HEIGHT_FN(lambda user, x, y: float(height(x, y)))
        self.lib.mefr_road_callback(self.handle, self._callback, None)

    # Operating conditions.
    def set_pressure(self, pascal):
        self.lib.mefr_set_pressure(self.handle, pascal)

    def set_tread_depth(self, metres):
        self.lib.mefr_set_tread_depth(self.handle, metres)

    def set_friction_scale(self, scale):
        self.lib.mefr_set_friction_scale(self.handle, scale)

    # Simulation (rot: 9 values, row-major rim-to-global).
    def reset(self, pos, rot, vel=(0, 0, 0), omega=(0, 0, 0)):
        self._check(self.lib.mefr_reset(self.handle, _arr(pos), _arr(rot), _arr(vel), _arr(omega)))

    def settle(self, pos, rot):
        self._check(self.lib.mefr_settle(self.handle, _arr(pos), _arr(rot)))

    def advance(self, pos, rot, vel, omega, dt):
        f = (ctypes.c_double * 3)()
        m = (ctypes.c_double * 3)()
        self._check(self.lib.mefr_advance(self.handle, _arr(pos), _arr(rot), _arr(vel), _arr(omega), dt, f, m))
        return list(f), list(m)

    def nodes(self):
        n = self.lib.mefr_node_count(self.handle)
        xyz = (ctypes.c_double * (3 * n))()
        torsion = (ctypes.c_double * n)()
        self.lib.mefr_nodes(self.handle, xyz, torsion)
        return [(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]) for i in range(n)], list(torsion)

    def blocks(self):
        n = self.lib.mefr_block_count(self.handle)
        out = (ctypes.c_double * (12 * n))()
        count = self.lib.mefr_blocks(self.handle, out, n)
        return [tuple(out[12 * i:12 * i + 12]) for i in range(count)]

    def contact(self):
        c = Contact()
        self.lib.mefr_contact_stats(self.handle, ctypes.byref(c))
        return c

    # Rigs.
    def steady(self, speed, load, slip_angle=0.0, slip_ratio=0.0, camber=0.0, free_rolling=None):
        if free_rolling is None:
            free_rolling = slip_ratio == 0.0
        forces = (ctypes.c_double * 6)()
        re = ctypes.c_double()
        self._check(self.lib.mefr_steady(self.handle, speed, load, slip_angle, slip_ratio, camber, 1 if free_rolling else 0, forces, ctypes.byref(re)))
        return list(forces) + [re.value]

    def report(self, out_dir, what="all", quick=True):
        self._check(self.lib.mefr_run_report(self.handle, what.encode(), out_dir.encode(), 1 if quick else 0))


def _demo(dll_folder):
    t = FlexRing(dll_folder=dll_folder)
    t.preprocess()
    for name, target, achieved in t.targets():
        print(f"{name:34s} target {target:10.4f} model {achieved:10.4f}")
    print("slip angle (deg)   Fy (N)   Mz (N m)")
    for a in (0.0, 1.0, 2.0, 4.0, 8.0):
        fx, fy, fz, mx, my, mz, re = t.steady(60 / 3.6, 4000.0, math.radians(a))
        print(f"{a:16.1f} {fy:8.1f} {mz:9.2f}")


if __name__ == "__main__":
    args = sys.argv[1:]
    folder = None
    if len(args) >= 2 and args[0] == "--dll":
        folder = args[1]
        args = args[2:]
    if args and args[0] == "demo":
        _demo(folder)
    else:
        print(__doc__)
