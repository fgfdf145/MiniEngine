"""Plots what miniengine_suspension_rig_report wrote.

    python plot_rig_report.py <report folder> <output png prefix>

Writes <prefix>_kc.png (K&C) and <prefix>_7post.png (seven-post). Needs numpy and matplotlib.
"""
import sys

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def load(folder, name):
    return np.genfromtxt(f'{folder}/{name}', delimiter=',', names=True)


def kc(folder, title, out):
    fig, axs = plt.subplots(2, 3, figsize=(14, 8))
    for axle, colour in (('front', 'C0'), ('rear', 'C1')):
        b = load(folder, f'kc_bounce_{axle}.csv')
        axs[0, 0].plot(b['travel_mm'], b['camber_l_deg'], colour, label=axle)
        axs[0, 1].plot(b['travel_mm'], b['toe_l_deg'], colour, label=axle)
        axs[0, 2].plot(b['travel_mm'], b['roll_center_mm'], colour, label=axle)
        r = load(folder, f'kc_roll_{axle}.csv')
        axs[1, 0].plot(r['roll_deg'], r['roll_moment_nm'] / 1000.0, colour, label=axle)
        axs[1, 1].plot(r['roll_deg'], r['camber_to_road_r_deg'], colour, label=f'{axle} outer (right)')
    s = load(folder, 'kc_steer.csv')
    axs[1, 2].plot(s['steering_wheel_deg'], s['left_deg'], label='left wheel')
    axs[1, 2].plot(s['steering_wheel_deg'], s['right_deg'], label='right wheel')
    labels = [
        ('wheel travel [mm] (+ bump)', 'camber to body [deg] (+ top out)', 'bounce: camber'),
        ('wheel travel [mm] (+ bump)', 'toe [deg] (+ toe-in)', 'bounce: toe (bump steer)'),
        ('wheel travel [mm] (+ bump)', 'roll centre above road [mm]', 'bounce: roll centre'),
        ('body roll [deg] (right side down)', 'roll moment [kNm]', 'roll: springs + anti-roll bar'),
        ('body roll [deg]', 'outer wheel camber to road [deg]', 'roll: camber'),
        ('steering wheel [deg]', 'wheel angle to the right [deg]', 'steering'),
    ]
    for ax, (x, y, t) in zip(axs.flat, labels):
        ax.set_xlabel(x)
        ax.set_ylabel(y)
        ax.set_title(t, fontsize=10)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle(f'{title}: virtual K&C rig')
    fig.tight_layout()
    fig.savefig(out, dpi=100)


def seven_post(folder, title, out):
    fig, axs = plt.subplots(2, 3, figsize=(14, 8))
    for mode, colour in (('heave', 'C0'), ('pitch', 'C1'), ('roll', 'C2')):
        d = load(folder, f'sweep_{mode}.csv')
        f = load(folder, f'sweep_{mode}_friction.csv')
        axs[0, 0].semilogx(d['frequency_hz'], d['body_gain'], colour, label=mode)
        axs[0, 0].semilogx(f['frequency_hz'], f['body_gain'], colour + '--', alpha=0.6)
        axs[0, 1].semilogx(d['frequency_hz'], d['body_phase_deg'], colour, label=mode)
        axs[0, 2].semilogx(d['frequency_hz'], d['load_var_fl'], colour, label=f'{mode} FL')
        axs[0, 2].semilogx(f['frequency_hz'], f['load_var_fl'], colour + '--', alpha=0.6)
    w = load(folder, 'sweep_warp.csv')
    axs[0, 2].semilogx(w['frequency_hz'], w['load_var_fl'], 'C3', label='warp FL')
    h = load(folder, 'sweep_heave.csv')
    axs[1, 0].semilogx(h['frequency_hz'], h['wheel_gain_fl'], label='front hub')
    axs[1, 0].semilogx(h['frequency_hz'], h['wheel_gain_rl'], label='rear hub')
    st = load(folder, 'step_heave.csv')
    axs[1, 1].plot(st['time_s'], st['body_mm'], label='body')
    a = load(folder, 'aero.csv')
    axs[1, 2].plot(a['downforce_n'] / 1000.0, a['front_mm'], label='front axle')
    axs[1, 2].plot(a['downforce_n'] / 1000.0, a['rear_mm'], label='rear axle')
    labels = [
        ('frequency [Hz]', 'body / pad displacement', 'sweep: body (dashed: assumed damper friction)'),
        ('frequency [Hz]', 'phase [deg]', 'sweep: body phase'),
        ('frequency [Hz]', 'tyre load amplitude / static', 'sweep: contact patch load (FL)'),
        ('frequency [Hz]', 'hub / pad displacement', 'heave sweep: hubs'),
        ('time [s]', 'body heave [mm]', 'heave step 10 mm'),
        ('downforce [kN] (45 % front)', 'ride height change [mm]', 'aero loaders'),
    ]
    for ax, (x, y, t) in zip(axs.flat, labels):
        ax.set_xlabel(x)
        ax.set_ylabel(y)
        ax.set_title(t, fontsize=10)
        ax.grid(alpha=0.3, which='both')
        ax.legend(fontsize=8)
    fig.suptitle(f'{title}: virtual seven-post rig')
    fig.tight_layout()
    fig.savefig(out, dpi=100)


if __name__ == '__main__':
    folder, prefix = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else 'car'
    kc(folder, title, prefix + '_kc.png')
    seven_post(folder, title, prefix + '_7post.png')
    print('wrote', prefix + '_kc.png', prefix + '_7post.png')
