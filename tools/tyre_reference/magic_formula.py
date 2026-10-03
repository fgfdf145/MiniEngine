"""Independent Python transcription of the steady-state Magic Formula, Pacejka, Tyre and Vehicle
Dynamics, 2nd ed. (2006), Section 4.3.2, Eqs. (4.E1-4.E78), "version 2004". It is a cross-check
for the C++ port in engine/tyre: written separately from the book, not from the C++.

Turn slip is left out (all zeta = 1), user scaling factors default to 1 (LMUV to 0), and slip
speed decay is off. Inputs: kappa, alpha (rad), gamma (rad), Fz (N), Vcx (m/s, default V0).
Outputs in ISO axes: Fx, Fy, Mx, My, Mz."""
import math
import re

EPS = 1e-6     # singularity guard, Pacejka p.185: "a small additional quantity epsilon"
EPS_V = 0.1    # Eq. (4.E6a)
A_MU = 10.0    # Eq. (4.E8)

SCALING = ['LFZO', 'LCX', 'LMUX', 'LEX', 'LKX', 'LHX', 'LVX', 'LCY', 'LMUY', 'LEY', 'LKY', 'LKYC',
           'LKZC', 'LHY', 'LVY', 'LTR', 'LRES', 'LXAL', 'LYKA', 'LVYKA', 'LS', 'LMX', 'LMY']


def read_tir(path):
    """KEY = value pairs of a .tir file, keys upper case, numbers only."""
    p = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = re.split(r'[$!]', line, maxsplit=1)[0].strip()
            if not line or line.startswith('['):
                continue
            key, _, value = line.partition('=')
            try:
                p[key.strip().upper()] = float(value.strip())
            except ValueError:
                pass
    return p


def sgn(x):
    return (x > 0) - (x < 0)


def guard(x, eps=EPS):
    """x + eps with the sign of x (Pacejka p.185)."""
    return x + (eps if x >= 0 else -eps)


def evaluate(par, kappa, alpha, gamma, Fz, Vcx=None):
    g = lambda k: par.get(k, 0.0)
    lam = lambda k: par.get(k, 1.0)
    R0, V0 = g('UNLOADED_RADIUS'), g('LONGVL')
    if Vcx is None:
        Vcx = V0
    out = dict(Fx=0.0, Fy=0.0, Mx=0.0, My=0.0, Mz=0.0)
    if Fz <= 0.0:
        return out

    Fz0p = lam('LFZO') * g('FNOMIN')                      # (4.E1)
    dfz = (Fz - Fz0p) / Fz0p                              # (4.E2)
    a = math.tan(alpha) * sgn(Vcx)                        # (4.E3) alpha*
    gs = math.sin(gamma)                                  # (4.E4) gamma*
    Vcy = -a * abs(Vcx)
    Vc = math.hypot(Vcx, Vcy)
    cosp = Vcx / (Vc + EPS_V)                             # (4.E6), (4.E6a)
    lmux_s, lmuy_s = lam('LMUX'), lam('LMUY')             # (4.E7) with LMUV = 0
    lmux_p = A_MU * lmux_s / (1 + (A_MU - 1) * lmux_s)    # (4.E8)
    lmuy_p = A_MU * lmuy_s / (1 + (A_MU - 1) * lmuy_s)

    # Longitudinal force, pure slip (4.E9-4.E18)
    SHx = (g('PHX1') + g('PHX2') * dfz) * lam('LHX')
    kx = kappa + SHx
    Cx = g('PCX1') * lam('LCX')
    mux = (g('PDX1') + g('PDX2') * dfz) * lmux_s
    Dx = mux * Fz
    Ex = min((g('PEX1') + g('PEX2') * dfz + g('PEX3') * dfz ** 2) * (1 - g('PEX4') * sgn(kx)) * lam('LEX'), 1.0)
    Kxk = Fz * (g('PKX1') + g('PKX2') * dfz) * math.exp(g('PKX3') * dfz) * lam('LKX')
    Bx = Kxk / guard(Cx * Dx)
    SVx = Fz * (g('PVX1') + g('PVX2') * dfz) * (abs(Vcx) / (EPS_V + abs(Vcx))) * lam('LVX') * lmux_p
    Fx0 = Dx * math.sin(Cx * math.atan(Bx * kx - Ex * (Bx * kx - math.atan(Bx * kx)))) + SVx

    # Lateral force, pure slip (4.E19-4.E30)
    Cy = g('PCY1') * lam('LCY')
    muy = (g('PDY1') + g('PDY2') * dfz) / (1 + g('PDY3') * gs ** 2) * lmuy_s
    Dy = muy * Fz
    Kya = (g('PKY1') * Fz0p * math.sin(g('PKY4') * math.atan(Fz / ((g('PKY2') + g('PKY5') * gs ** 2) * Fz0p)))
           / (1 + g('PKY3') * gs ** 2) * lam('LKY'))
    By = Kya / guard(Cy * Dy)
    SVyg = Fz * (g('PVY3') + g('PVY4') * dfz) * gs * lam('LKYC') * lmuy_p
    SVy = Fz * (g('PVY1') + g('PVY2') * dfz) * lam('LVY') * lmuy_p + SVyg
    Kyg0 = Fz * (g('PKY6') + g('PKY7') * dfz) * lam('LKYC')
    SHy = (g('PHY1') + g('PHY2') * dfz) * lam('LHY') + (Kyg0 * gs - SVyg) / guard(Kya)
    ay = a + SHy
    Ey = min((g('PEY1') + g('PEY2') * dfz) * (1 + g('PEY5') * gs ** 2 - (g('PEY3') + g('PEY4') * gs) * sgn(ay)) * lam('LEY'), 1.0)
    Fy0 = Dy * math.sin(Cy * math.atan(By * ay - Ey * (By * ay - math.atan(By * ay)))) + SVy

    # Aligning torque, pure slip (4.E31-4.E49)
    SHt = g('QHZ1') + g('QHZ2') * dfz + (g('QHZ3') + g('QHZ4') * dfz) * gs
    at = a + SHt
    Kyap = guard(Kya)
    SHf = SHy + SVy / Kyap
    ar = a + SHf
    Bt = (g('QBZ1') + g('QBZ2') * dfz + g('QBZ3') * dfz ** 2) * (1 + g('QBZ5') * abs(gs) + g('QBZ6') * gs ** 2) * lam('LKY') / lmuy_s
    Ct = g('QCZ1')
    Dt0 = Fz * (R0 / Fz0p) * (g('QDZ1') + g('QDZ2') * dfz) * lam('LTR') * sgn(Vcx)
    Dt = Dt0 * (1 + g('QDZ3') * abs(gs) + g('QDZ4') * gs ** 2)
    Et = min((g('QEZ1') + g('QEZ2') * dfz + g('QEZ3') * dfz ** 2)
             * (1 + (g('QEZ4') + g('QEZ5') * gs) * (2 / math.pi) * math.atan(Bt * Ct * at)), 1.0)
    Br = g('QBZ9') * lam('LKY') / lmuy_s + g('QBZ10') * By * Cy
    Cr = 1.0
    Dr = (Fz * R0 * ((g('QDZ6') + g('QDZ7') * dfz) * lam('LRES') + (g('QDZ8') + g('QDZ9') * dfz) * gs * lam('LKZC')
                     + (g('QDZ10') + g('QDZ11') * dfz) * gs * abs(gs)) * cosp * lmuy_s * sgn(Vcx))

    def trail(x):
        return Dt * math.cos(Ct * math.atan(Bt * x - Et * (Bt * x - math.atan(Bt * x)))) * cosp

    t0 = trail(at)
    Mz0 = -t0 * Fy0 + Dr * math.cos(Cr * math.atan(Br * ar))

    # Longitudinal force, combined slip (4.E50-4.E57)
    Bxa = (g('RBX1') + g('RBX3') * gs ** 2) * math.cos(math.atan(g('RBX2') * kappa)) * lam('LXAL')
    Cxa = g('RCX1')
    Exa = min(g('REX1') + g('REX2') * dfz, 1.0)
    SHxa = g('RHX1')
    aS = a + SHxa
    G = lambda B, C, E, x: math.cos(C * math.atan(B * x - E * (B * x - math.atan(B * x))))
    Gxa = G(Bxa, Cxa, Exa, aS) / G(Bxa, Cxa, Exa, SHxa)
    Fx = Gxa * Fx0

    # Lateral force, combined slip (4.E58-4.E67)
    DVyk = muy * Fz * (g('RVY1') + g('RVY2') * dfz + g('RVY3') * gs) * math.cos(math.atan(g('RVY4') * a))
    SVyk = DVyk * math.sin(g('RVY5') * math.atan(g('RVY6') * kappa)) * lam('LVYKA')
    SHyk = g('RHY1') + g('RHY2') * dfz
    kS = kappa + SHyk
    Byk = (g('RBY1') + g('RBY4') * gs ** 2) * math.cos(math.atan(g('RBY2') * (a - g('RBY3')))) * lam('LYKA')
    Cyk = g('RCY1')
    Eyk = min(g('REY1') + g('REY2') * dfz, 1.0)
    Gyk = G(Byk, Cyk, Eyk, kS) / G(Byk, Cyk, Eyk, SHyk)
    Fy = Gyk * Fy0 + SVyk

    # Overturning couple (4.E69) and rolling resistance moment (4.E70)
    Mx = Fz * R0 * (g('QSX1') - g('QSX2') * gs + g('QSX3') * Fy / Fz0p) * lam('LMX')
    Vr = Vcx + kappa * abs(Vcx)  # V_r = V_cx - V_sx with V_sx = -kappa |V_cx|
    My = -Fz * R0 * (g('QSY1') * math.atan(Vr / V0) + g('QSY2') * Fx / Fz0p) * lam('LMY')

    # Aligning torque, combined slip (4.E71-4.E78)
    ratio = Kxk / Kyap
    at_eq = math.sqrt(at ** 2 + (ratio * kappa) ** 2) * sgn(at)
    ar_eq = math.sqrt(ar ** 2 + (ratio * kappa) ** 2) * sgn(ar)
    t = trail(at_eq)
    Fyp = Fy - SVyk
    Mzr = Dr * math.cos(Cr * math.atan(Br * ar_eq))
    s = R0 * (g('SSZ1') + g('SSZ2') * (Fy / Fz0p) + (g('SSZ3') + g('SSZ4') * dfz) * gs) * lam('LS')
    Mz = -t * Fyp + Mzr + s * Fx

    out.update(Fx=Fx, Fy=Fy, Mx=Mx, My=My, Mz=Mz, Fx0=Fx0, Fy0=Fy0, Mz0=Mz0, Kxk=Kxk, Kya=Kya, t=t, mux=mux, muy=muy)
    return out
