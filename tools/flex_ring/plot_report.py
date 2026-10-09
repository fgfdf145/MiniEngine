"""Plots what `miniengine_flex_ring report --out <folder>` wrote, in the layout of the FTire papers' figures.

    python plot_report.py <report folder> <output png prefix>

Writes <prefix>_static.png (Fz-deflection, footprint), <prefix>_steady.png (Gipser 1999 figs. 7-12: Fx(kappa),
Fy(alpha), Mz(alpha) at Fz = 2, 4, 6, 8 kN, combined slip, camber) and <prefix>_cleat.png (figs. 5, 6: wheel load
and longitudinal force over cleats at 40, 80, 120 km/h, fixed spindle). Needs matplotlib.
"""
import csv
import sys
from collections import OrderedDict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def rows(folder, name):
    with open(f"{folder}/{name}", newline="") as f:
        return list(csv.DictReader(f))


def series(table, key):
    groups = OrderedDict()
    for r in table:
        groups.setdefault(r[key], []).append(r)
    return groups


def static(folder, out):
    s = rows(folder, "static_fz_deflection.csv")
    fp = rows(folder, "footprint.csv")
    fig, axs = plt.subplots(1, 2, figsize=(13, 5))
    axs[0].plot([float(r["deflection_mm"]) for r in s], [float(r["wheel_load_n"]) for r in s], "C1-o", ms=3, label="model")
    targets = rows(folder, "preprocess_targets.csv")
    data = {t["name"]: float(t["target"]) for t in targets}
    pts = []
    tir = open(f"{folder}/tyre_data.tir").read().splitlines()
    vals = {}
    for line in tir:
        line = line.split("$")[0]
        if "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
    try:
        pts.append((float(vals["first_deflection"]), data["stat_wheel_load_at_first_defl"]))
        if "stat_wheel_load_at_second_defl" in data:
            pts.append((float(vals["second_deflection"]), data["stat_wheel_load_at_second_defl"]))
    except KeyError:
        pass
    if pts:
        axs[0].plot([p[0] for p in pts], [p[1] for p in pts], "ks", label="data")
    axs[0].set_xlabel("deflection (mm)")
    axs[0].set_ylabel("wheel load (N)")
    axs[0].set_title("Radial characteristic, flat road")
    axs[0].grid(True, alpha=0.3)
    axs[0].legend()
    sc = axs[1].scatter([float(r["x_mm"]) for r in fp], [float(r["y_mm"]) for r in fp], c=[float(r["pressure_bar"]) for r in fp], s=10, cmap="viridis")
    fig.colorbar(sc, ax=axs[1], label="ground pressure (bar)")
    axs[1].set_aspect("equal")
    axs[1].set_xlabel("x (mm)")
    axs[1].set_ylabel("y (mm)")
    axs[1].set_title("Footprint at the first deflection")
    fig.tight_layout()
    fig.savefig(out, dpi=110)


def steady(folder, out):
    groups = series(rows(folder, "steady_state.csv"), "series")
    fig, axs = plt.subplots(2, 3, figsize=(16, 9))

    def plot(ax, prefix, x, y, xscale=1.0, label_from=None):
        for name, pts in groups.items():
            if not name.startswith(prefix):
                continue
            ax.plot([float(p[x]) * xscale for p in pts], [float(p[y]) for p in pts], "-o", ms=2.5, label=name[len(prefix):])
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=8)

    plot(axs[0, 0], "Fx_kappa ", "slip_ratio", "fx_n", 100.0)
    axs[0, 0].set(xlabel="long. slip (%)", ylabel="Fx (N)", title="Fore-aft force (fig. 7)")
    plot(axs[0, 1], "Fy_Mz_alpha ", "slip_angle_deg", "fy_n")
    axs[0, 1].set(xlabel="side slip angle (deg)", ylabel="Fy (N)", title="Side force (fig. 8)")
    plot(axs[0, 2], "Fy_Mz_alpha ", "slip_angle_deg", "mz_nm")
    axs[0, 2].set(xlabel="side slip angle (deg)", ylabel="Mz (N m)", title="Aligning torque (fig. 9)")
    plot(axs[1, 0], "combined ", "slip_ratio", "fx_n", 100.0)
    for name, pts in groups.items():
        if name.startswith("combined "):
            axs[1, 0].plot([float(p["slip_ratio"]) * 100.0 for p in pts], [float(p["fy_n"]) for p in pts], "--", lw=1)
    axs[1, 0].set(xlabel="long. slip (%)", ylabel="Fx (solid), Fy (dashed) (N)", title="Combined slip at 4 kN (fig. 10)")
    plot(axs[1, 1], "camber ", "slip_angle_deg", "fy_n")
    axs[1, 1].set(xlabel="side slip angle (deg)", ylabel="Fy (N)", title="Side force with camber, 4 kN (fig. 11)")
    plot(axs[1, 2], "camber ", "slip_angle_deg", "mz_nm")
    axs[1, 2].set(xlabel="side slip angle (deg)", ylabel="Mz (N m)", title="Aligning torque with camber, 4 kN (fig. 12)")
    fig.tight_layout()
    fig.savefig(out, dpi=110)


def cleat(folder, out):
    groups = series(rows(folder, "cleat.csv"), "run")
    shapes = OrderedDict()
    for name in groups:
        shape = name.rsplit(" ", 2)[0]
        shapes.setdefault(shape, []).append(name)
    fig, axs = plt.subplots(2, len(shapes), figsize=(6 * len(shapes), 8), squeeze=False)
    for col, (shape, names) in enumerate(shapes.items()):
        for name in names:
            pts = groups[name]
            t0 = None
            ts, fz, fx = [], [], []
            for p in pts:
                if float(p["position_m"]) < -0.08:
                    continue
                t = float(p["time_s"])
                t0 = t if t0 is None else t0
                ts.append(t - t0)
                fz.append(float(p["fz_n"]))
                fx.append(float(p["fx_n"]))
            label = name[len(shape) + 1:]
            axs[0, col].plot(ts, fz, label=label)
            axs[1, col].plot(ts, fx, label=label)
        axs[0, col].set(title=f"{shape}: wheel load", xlabel="time from the rim 80 mm before the cleat (s)", ylabel="Fz (N)")
        axs[1, col].set(title=f"{shape}: longitudinal force", xlabel="time (s)", ylabel="Fx (N)")
        for r in range(2):
            axs[r, col].grid(True, alpha=0.3)
            axs[r, col].legend(fontsize=8)
            axs[r, col].set_xlim(0.0, 0.1)
    fig.tight_layout()
    fig.savefig(out, dpi=110)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    folder, prefix = sys.argv[1], sys.argv[2]
    static(folder, f"{prefix}_static.png")
    steady(folder, f"{prefix}_steady.png")
    cleat(folder, f"{prefix}_cleat.png")
