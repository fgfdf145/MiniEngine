# Assetto Corsa Car Data

## Goal

An imported Assetto Corsa car drives on its own figures. Until now the kn5 import took the model and
nothing else: mass, torque, gearbox, brakes and springs came from the Vehicle panel's defaults (1400 kg,
500 Nm flat, a five-speed box), whatever the car was. A car's physics lives in `data.acd` beside its kn5;
this reads it.

## What a car ships

`ks_porsche_718_boxster_s_pdk/data.acd` holds 49 files (`car.ini`, `engine.ini`, `power.lut`,
`drivetrain.ini`, `brakes.ini`, `suspensions.ini`, `tyres.ini`, `aero.ini`, `electronics.ini`, ...). Each
byte is stored as a 32-bit word with a key added; the key is eight numbers joined by dashes
(`6-105-61-232-126-93-15-108`, repeated over the file) that the game derives from the car's **folder
name**.

## Decisions

1. **The key: six numbers computed, one searched.** The first six and the eighth follow from the name
   (sum of the characters, alternating differences, a division recurrence, ...; see `KeyNumbers`). The
   seventh is found by trying all 256 values and keeping the one under which the archive's ini and lut
   files decrypt to the most text (at least 97% of bytes, and 0.2 points over the runner-up). The
   formulas were fitted to keys recovered statistically from 42 cars and then checked against all 178
   archives of the base game; the seventh's formula was not found, hence the search. Two catches on the
   way: the fifth number starts from 66, not 2 (they differ by one high bit, which four of 178 cars
   show), and the search key must be tried against real text, not just printable bytes (a wrong seventh
   number decrypts to plausible text in the first 200 bytes).
2. **A renamed folder does not open**, exactly as in the game. The import then finishes without the car's
   figures and reports why (`Kn5ImportReport::carDataProblem`).
3. **An unpacked `data/` folder** (mod authors work in one) is read the same way, without a key.
4. **`VehicleCarSpec`** (in `vehicle_settings.h`) is the physics-facing result: SI units, every field
   optional. The AC to SI conversions are in `AcCarData::BuildSpec`; the physics library knows nothing of
   AC.
5. **`MINIENGINE_vehicle`** carries the spec on the glTF document. The loader reads it into
   `LoadedModelData::carSpec`; `ApplyCarSpec` lays it over the tuning, which is what the Vehicle panel's
   "Use the Car's Own Data" (default on) selects.

## Conversions

| Spec | From | How |
|---|---|---|
| mass | `car.ini` BASIC TOTALMASS | as is (with driver) |
| drive | `drivetrain.ini` TRACTION TYPE | RWD, FWD, AWD*, AWD2 is all-wheel |
| torque curve | `power.lut` x (1 + turbo boost) | the boost of each `TURBO_n`: MAX_BOOST capped by WASTEGATE, times `(rpm/REFERENCE_RPM)^GAMMA` up to 1; the curve's peak is the engine's torque, the shape is Jolt's `mNormalizedTorque` |
| revs | `engine.ini` LIMITER, MINIMUM | |
| gearbox | `drivetrain.ini` GEAR_n, GEAR_R, FINAL | Jolt's gear ratios and the differential ratio; auto shifts at 88% of the limiter up and 30% down |
| steering | `car.ini` STEER_LOCK / STEER_RATIO | the front wheels' lock (26.7 degrees on the Boxster); the steering wheel turns STEER_LOCK each way |
| brakes | `brakes.ini` MAX_TORQUE, FRONT_SHARE, HANDBRAKE_TORQUE | each wheel takes MAX_TORQUE times its axle's share (FRONT_SHARE or the rest), so the four take twice MAX_TORQUE; HANDBRAKE_TORQUE on each rear wheel. (Read as the whole car's torque, road cars could not lock a tyre: corrected 2026-10-05.) |
| springs | `suspensions.ini` SPRING_RATE, HUB_MASS, CG_LOCATION | natural frequency of one wheel's sprung mass (1.78 Hz on the Boxster); motion ratio ignored |
| dampers | DAMP_BUMP, DAMP_REBOUND | their mean over critical damping (0.78) |
| ARB, LSD | `[ARB]`, `[DIFFERENTIAL]` POWER/COAST | on or off; Jolt's locking ratio is fixed |
| engine inertia | `engine.ini` INERTIA | Jolt's `mInertia` (0.137 kg m^2 on the Boxster against Jolt's 0.5) |
| gear change | `drivetrain.ini` CHANGE_UP_TIME | Jolt's `mSwitchTime`, the seconds with no torque (30 ms; Jolt's is 0.5 s) |
| clutch | `[AUTOCLUTCH]` UPSHIFT_PROFILE | `mClutchReleaseTime`: the profile's last point, or 0.1 s for NONE (Jolt's is 0.3 s) |
| tyre grip | `tyres.ini` DX_REF, DY_REF, FZ0, LS_EXPX, LS_EXPY | the friction at the load one wheel carries at rest: `REF * (load/FZ0)^(EXP-1)` (1.31 front, 1.29 rear on the Boxster's semislicks), or DX0 + DX1 without those keys |
| tyre slip | FRICTION_LIMIT_ANGLE, FALLOFF_LEVEL | the slip angle of the peak (7.5 degrees), the slip ratio from it (its tangent), the share of the peak left past it (0.86) |
| wheel inertia | ANGULAR_INERTIA | the wheel's `mInertia` (1.62 front, 1.97 rear; Jolt's is 0.9) |
| air | `aero.ini` wings with their LUTs | drag area `Cd(angle) * gain * chord * span` and downforce area from the lift coefficient, each as a force at the wing's position from the centre of mass; the body's linear damping (Jolt's 0.05 stand-in for drag) goes to 0 |

Kept whole but not used by the physics yet: every tyre compound with every number of its sections and its
curves (wear, temperature), the wings' other curves and zone modifiers and the controllers that move them
(the Boxster's spoiler rises above 120 km/h), the turbos (lag), engine braking, the gearbox's change-down and
ignition-cut times, the clutch's torque limit and window, the autoclutch profiles, the differential's
lock and preload, and the driver aids (ABS, traction control, EDL) with their limits.

## Out of scope

AC's tyre model itself (thermal, wear, pressure, camber: only the peak grip, slip peaks and inertia reach
Jolt's wheel friction), turbo lag, a controlled wing angle, ABS, traction control and EDL as behaviour (their
data is carried), the damage model, `.ksanim` animations, sound banks, driver position, `lights.ini`, and
the car's own `collider.ini`.

## Measured

- All 178 installed cars of the base game decrypt and give a full spec (mass, drive, torque curve, gears);
  `MINIENGINE_AC_CARS=<...>/content/cars miniengine_kn5_import_tests` runs that check. Two cars
  (`ks_ferrari_488_challenge_evo`, `ks_ferrari_488_gt3_2020`) ship no `data.acd` and no `data/`.
- The Boxster imports as 1460 kg, RWD, 386 Nm (2000 to 4500 rpm), 7500 rpm, seven gears, 26.7 degree
  lock, 800 Nm brakes per wheel at 65% front, 1.78 Hz springs.
- **Acceleration (second pass).** On flat tarmac 0 to 100 km/h took 9.0 s on the first pass's data
  (the default tuning: 6.5 s; the real car 4.2 s). With the launch, grip and air below it takes 5.3 s.
  What each part is worth, taken one at a time from the 9.0 s: the engine's inertia 1.2 s (Jolt's 0.5
  kg m^2 makes the engine reflect 865 kg through a 14:1 first gear), the dual clutch's shift times 0.8 s
  (Jolt loses half a second of torque at every change), both together 2.2 s, the tyres 0.6 s more, and the
  body's linear damping (5% of the speed lost each second, several times the real drag at 100 km/h) 1.0 s.
  The physics engine's own launch, not a launch rpm, was the trouble: the revs climb to 2600 within a
  quarter second with the real inertia.
- What is left (1 s): in first gear the engine has more torque than the tyres can hold (15.7 kN at the
  wheels against about 11 kN of grip), so the driven wheels spin while the engine sits on its limiter and
  the gearbox refuses to change up (Jolt does not shift while a wheel slips), for 1.75 s. Two traction
  controls were tried and dropped: cutting the throttle on rim slip (60 Hz cannot hold a wheel that
  gains 4 m/s of rim speed in one step) and holding the torque to what the load and grip allow (same 0-100
  time, a smoother first gear).

## Automated Verification

- `AcdKeysMatchTheGame`: keys of real archives (Boxster, abarth500, bmw_m3_e30, lotus_49, p4-5_2011).
- `AcdArchiveDecryptsAndRefusesAWrongFolder`: round trip for four values of the seventh number, both
  layouts, another case of the folder name; a renamed folder, a truncated archive and garbage throw.
- `CarDataBecomesASpec`: every conversion above on the Boxster's figures; missing files leave fields out.
- `ImportWritesTheCarsOwnData`: no data, a `data.acd`, one for another folder (import survives, problem
  reported), an unpacked `data/`; the figures survive the glTF and the loader.
- `TestCarSpecReplacesWhatItKnows`, `TestCarOnItsOwnDataAccelerates` (bounds 0-100 to 3.5..6.5 s, and a heavy
  engine or a slow gear change each cost a launch), `TestTyreGripSetsAcceleration` (grippy against hard
  tyres; the tyre's friction times the surface's, not the square root), `TestAerodynamicsDragsAndPressesDown`
  (physics).
- The asset tests carry the launch, clutch, tyre, wing and driver-aid data through the archive, the glTF and
  the loader; every installed car must have tyres, wings and an engine inertia.

## Manual Acceptance (by drive)

Re-import the Boxster (an import never overwrites: import it under a new name or delete
`assets/models/porsche_boxster_s_pdk`). Drive it: the Vehicle panel shows "Car's own data: 1460 kg, RWD,
386 Nm, 7500 rpm, 7 gears, brakes, springs"; the gear readout goes up to 7 with the revs falling at each
change; the steering wheel turns 400 degrees at full lock; switching "Use the Car's Own Data" off gives
the old car.
