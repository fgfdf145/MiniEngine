# Realistic Vehicle Dynamics

## Goal

Drive an imported Assetto Corsa car so that its tyres, suspension and drivetrain behave like the car's own
data says, close enough to the game that its known figures (0-100, braking distance, lateral g, roll
gradient) can be used as tests. Today the car runs on Jolt Physics' `WheeledVehicleController`, which is a
game-grade model (Marco Monster's "Car Physics for Games"). This document records what it does, where it
cannot follow real behaviour, and the plan to go beyond it.

## What runs today (measured, 2026-09-30)

Jolt 5.5.0 (source in `.deps/vcpkg/buildtrees/joltphysics/src/v5.5.0-ca68e48c31.clean/Jolt/Physics/Vehicle`).

**Tyre.** Per wheel and step: slip ratio `|(w r - v) / v|` looked up in a piecewise-linear curve for the
longitudinal friction, slip angle `acos(|v_long| / |v|)` looked up in another for the lateral. The curves
are three points each (`ApplyTyres` in `physics_world.cpp`): 0, the peak, and a fall to
`peak * postPeakShare` at 3.3x (ratio) or 6.7x (angle) the peak's slip. The force is not a function of the
slip: each direction is a velocity constraint that tries to cancel the slip in one step, its impulse clamped to
`mu(slip) * N * dt`, `N` the suspension's impulse. Friction is `mu_tyre * mu_surface` when tyre data is set
(`SetCombineFriction`), Jolt's square root otherwise.

**Suspension.** One linear spring and damper per wheel along the body's down axis, `k = m/4 w^2`,
`c = 2 zeta m/4 w` (stiffness mode since the sag fix: the frequency mode scales by the body's effective mass
and sagged 10 to 25% less than `ComputeRestSuspensionLength`). Pushes only, hard velocity stop at the minimum
length. Travel `clamp(radius, 0.15, 0.35)`. The wheel is not a body: its position is the ground contact and
the suspension length.

**Braking.** The brake torque only slows the wheel (`w -= T dt / I`); the friction constraint then pulls it
back towards the ground's speed. Locked when `T > w I / dt`. So the steady slip under braking is set by
`T dt / (I w)` and the step, not by the tyre: about 0.2 on the Boxster at 800 Nm.

**Drive.** Engine -> clutch (`strength * (w_engine - w_wheels * ratio)`, no torque limit; the engine's rpm
is clamped at idle) -> gearbox -> differential. Jolt's limited slip gives all torque to the slower wheel past a
ratio.

## What it cannot do

| Real behaviour | Why Jolt's model cannot |
|---|---|
| Braking slip set by the tyre, ABS meaningful | slip set by `T dt / I` (an ABS that cut the brake made stops longer: 59 -> 70..97 m) |
| Clutch slips on a start | clutch torque is `strength * dw`, the idle rpm drags the wheels: rear tyres at 2-6x ground speed |
| Combined slip (friction circle) | longitudinal and lateral limits are independent |
| Load sensitivity, camber, relaxation length, temperature, pressure, wear | one `mu` per curve, force linear in load, response instantaneous |
| Anti-roll bar | `VehicleAntiRollBar::mStiffness` defaults to 1000 N/m; we do not set it (3% of a 31 kN/m wheel rate) |
| Separate bump and rebound dampers, progressive rate, bump stops | one linear spring and one damper |
| Unsprung mass, wheel hop, tyre vertical stiffness | the wheel follows the ground kinematically |
| Suspension kinematics (camber gain, roll centre, anti-dive) | straight-line suspension, fixed camber and toe |

## What has been done in the working tree (not committed, see below)

Physics: static sag matches `ComputeRestSuspensionLength` (stiffness mode); `VehicleWheelState` reports mount,
suspension axis and travel, contact point and normal, load, longitudinal and lateral force, slip ratio and
angle, friction and peak friction, brake torque, and the wheel's roll angle; brake torque shared by the load
each wheel carries (`dynamicBrakeBias`); wheel roll followed by unwrapped angle (the pose slerp ran backwards
past 188 rad/s); traction control by slipping the clutch past what the driven tyres hold
(`tractionControlGrip`, default 0.85); a clutch-pack limited slip differential replacing Jolt's snap
(`limitedSlipLock`). Editor: a physics overlay (springs with a travel gauge, tyre outlines and contact
patches coloured by grip use, force arrows, a friction circle per wheel) in the Vehicle panel.

Baselines to beat (Boxster S PDK data, flat ground, 60 Hz):

- 0-100 km/h 5.60 s with traction control (5.22 s without, 4.2 s real); rear tyres' speed over the ground's:
  mean +0.13, peak +1.0..2.2, first second 1.4x.
- 100 km/h to rest 59 m (real about 35 m), front tyres locked 0.4 s, stopping limited by 800 Nm per wheel.
- 90 km/h, steering 0.3: mean slip angle 9 degrees, peak 15.
- Default box car, 25 m/s to rest: 44.9 m.

## Plan

**Phase 0: what Jolt allows** (small, keeps everything else)
1. Anti-roll bar stiffness from `suspensions.ini` `ARB` (or a share of the wheel rate); per axle.
2. Spring frequency and damping per axle; today both axles get one average.
3. Bump and rebound damping and a progressive bump stop, by rewriting the spring's stiffness and damping each
   step (the same way brake torque and clutch strength are already written each step).
4. `WheeledVehicleController::SetTireMaxImpulseCallback`: a friction ellipse from the slip values it is given,
   and load sensitivity (`mu` falling with load, from `tyres.ini`).

**Phase 1: our own wheel and tyre solver**
- Each wheel has its own angular velocity in its equation of motion: drive torque, brake torque and the road's
  reaction. Integrate it in sub-steps.
- Tyre force from a combined-slip model (Pacejka MF5.2 reduced, or a brush model) with load sensitivity and
  a relaxation length (first-order lag on the slip). Applied to the Jolt body at the contact patch.
- Keep `VehicleWheelState` as the interface; the overlay and the wheel animation do not change.

**Phase 2: suspension with unsprung mass**
- A vertical degree of freedom per wheel: unsprung mass, tyre vertical stiffness and damping.
- Spring rate with progression, packer and bump stop, fast and slow bump and rebound dampers, motion ratio,
  anti-roll bar, camber and toe as a function of travel, static camber.

**Phase 3: drivetrain**
- Engine torque map, inertia and limiter; a clutch with a torque capacity (a start slips, the idle drag goes);
  gearbox shift logic; differential power, coast and preload; then the electronics (ABS, traction control) as
  the car's own data defines them, and remove the clutch-based traction control added as a stand-in.

## Engineering notes

- **Sub-steps.** `PhysicsSystem::Update(dt, collisionSteps)` runs step listeners every collision sub-step;
  4 sub-steps of 60 Hz give 240 Hz. A 30 kg unsprung mass on a 200 kN/m tyre rings at about 13 Hz, which
  an explicit step of 4 ms integrates. The fixed step stays 1/60 s, so the interpolation of poses stays.
- **Where it plugs in.** Keep `VehicleConstraint`'s wheel collision (cylinder cast) or move to our own
  `NarrowPhaseQuery` casts; replace `WheeledVehicleController`. `PhysicsWorld`'s public API (poses, wheel
  states, telemetry, controls) stays the same.
- **Data.** `tyres.ini` (peak grip `DX_REF`/`DY_REF`, `FZ0`, load sensitivity exponents, relaxation length,
  slip limits, rolling resistance), `suspensions.ini` (spring, progressive rate, bump stop, packer, dampers with
  fast knees, `ARB`, geometry, camber, toe), `drivetrain.ini`, `electronics.ini`. All are already carried whole
  in `MINIENGINE_vehicle`; the physics reads a handful. AC's internal tyre formula is not public, so the model
  is fitted to these anchors and checked against the car's known figures.
- **Validation.** Benchmarks on the vehicle test track (`car_test_track`): skidpad lateral g, 100 km/h braking
  distance, 0-100, roll gradient (degrees per g), understeer gradient, ride frequency; plus the overlay's
  friction circle to see the tyres work in a sensible range.

## Open decisions (the user's)

1. Target: "close to the game" (Phases 0-3 with the tyre and suspension as above) or engineering grade (add tyre
   temperature, pressure, wear and full suspension geometry).
2. Whether to remove Jolt's `WheeledVehicleController` entirely (Phase 1) or keep it for the drivetrain until
   Phase 3.
3. Which slip the user wants solved first: braking lock, cornering slide, or the launch's first metres.

## Not done

Wheel and tyre temperature, wear and pressure; surface effects beyond grip (rumble strips, damping); tyre
noise; damage; the flat-spot and tyre-carcass dynamics.
