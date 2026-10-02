"""Reference camber/toe-vs-travel points for the C++ suspension tests, from the closed-form
solutions (Rill 5.4 for the double wishbone, doc section 2C for the MacPherson), which were checked
against the book and against an independent Newton solve. Frame: x forward, y left (outward for a
left wheel), z up. Signs: camber positive top-outward, toe positive toe-in."""
import numpy as np, importlib.util, io, contextlib, os
here = os.path.dirname(os.path.abspath(__file__))
def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(here, name + '.py'))
    m = importlib.util.module_from_spec(spec)
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(m)
    return m
def attitude(axis):
    return -np.degrees(np.arcsin(axis[2])), np.degrees(np.arctan2(axis[0], axis[1]))

rill = load('rill_double_wishbone')
ey = np.array([0, 1, -0.8 / 180 * np.pi]); ey /= np.linalg.norm(ey)
print('// Rill 2012, Listing 5.6 (front left double wishbone), wheel axis [0, 1, -0.8 deg in rad]')
print('// {travel, rack, wheel x, camber deg, toe deg}')
for ph, u in [(-10, 0), (-5, 0), (5, 0), (10, 0), (0, -0.04), (0, 0.04), (5, 0.03)]:
    avw, w, de, _ = rill.kin(np.radians(ph), u)
    c, t = attitude(avw @ ey)
    print(f'{{{w[2]:.12f}, {u}, {w[0]:.12f}, {c:.10f}, {t:.10f}}},')

mp = load('macpherson_kinematics')
for name, sec in (('front', mp.F), ('rear', mp.Rr)):
    P = mp.acset(sec); s = mp.Strut(P)
    print(f'// Boxster {name} (AC suspensions.ini, STRUT), wheel axis [0, 1, 0]')
    cases = [(-15, 0), (-8, 0), (8, 0), (15, 0)] + ([(0, -0.03), (0, 0.03), (8, 0.02)] if name == 'front' else [])
    for ph, u in cases:
        prev = 0
        R, C, lam, de = s.solve(np.radians(ph), u, prev)
        w = C + R @ (P['W'] - P['C'])
        c, t = attitude(R @ np.array([0, 1.0, 0]))
        print(f'{{{w[2]:.12f}, {u}, {w[0]:.12f}, {c:.10f}, {t:.10f}, {lam:.12f}}},')
