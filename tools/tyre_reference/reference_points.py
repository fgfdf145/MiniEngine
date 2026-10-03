"""Reference Magic Formula points for the C++ tyre tests, from the independent Python transcription
(magic_formula.py) evaluated on the Pacejka (2006) Table A3.1 tyre. Prints a C++ initializer list:
{kappa, alpha rad, gamma rad, Fz N, Vcx m/s, Fx, Fy, Mz, My}."""
import math
import os

import magic_formula as mf

here = os.path.dirname(os.path.abspath(__file__))
tir = os.path.join(here, '..', '..', 'tests', 'fixtures', 'tyres', 'pacejka2006_205_60R15.tir')
par = mf.read_tir(tir)
deg = math.pi / 180

cases = []
for Fz in (2000.0, 4000.0, 6000.0):
    for k in (-0.5, -0.1, -0.02, 0.0, 0.02, 0.05, 0.1, 0.3, 1.0):
        cases.append((k, 0.0, 0.0, Fz, None))
    for a in (-15, -6, -2, -0.5, 0.5, 2, 6, 15):
        cases.append((0.0, a * deg, 0.0, Fz, None))
for k, a in ((0.05, 2), (0.1, 4), (-0.1, 4), (-0.3, -8), (0.2, -12)):
    cases.append((k, a * deg, 0.0, 4000.0, None))
for g in (-4, 3, 6):
    for k, a in ((0.0, 0.0), (0.0, 3), (0.08, -5)):
        cases.append((k, a * deg, g * deg, 4000.0, None))
cases.append((0.05, 3 * deg, 2 * deg, 3000.0, 5.0))
cases.append((-0.05, 3 * deg, 0.0, 4000.0, -10.0))

print('// tools/tyre_reference/reference_points.py')
print('// {kappa, alpha, gamma, Fz, Vcx, Fx, Fy, Mz, My}')
for k, a, g, Fz, V in cases:
    V = par['LONGVL'] if V is None else V
    o = mf.evaluate(par, k, a, g, Fz, V)
    print(f'{{{k!r}, {a!r}, {g!r}, {Fz!r}, {V!r}, {o["Fx"]!r}, {o["Fy"]!r}, {o["Mz"]!r}, {o["My"]!r}}},')

o = mf.evaluate(par, 0.0, 0.0, 0.0, 4000.0)
print(f'// at Fz0: Kxk {o["Kxk"]:.3f} N, Kya {o["Kya"]:.3f} N/rad ({o["Kya"] * deg:.2f} N/deg), mux {o["mux"]:.4f}, muy {o["muy"]:.4f}, trail {o["t"] * 1000:.2f} mm')
