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
| brakes | `brakes.ini` MAX_TORQUE, FRONT_SHARE, HANDBRAKE_TORQUE | the total over four wheels, split by the front's share; the hand brake over the rear two |
| springs | `suspensions.ini` SPRING_RATE, HUB_MASS, CG_LOCATION | natural frequency of one wheel's sprung mass (1.78 Hz on the Boxster); motion ratio ignored |
| dampers | DAMP_BUMP, DAMP_REBOUND | their mean over critical damping (0.78) |
| ARB, LSD | `[ARB]`, `[DIFFERENTIAL]` POWER/COAST | on or off; Jolt's locking ratio is fixed |

## Out of scope

`tyres.ini` (AC's tyre model has no counterpart in Jolt's wheel friction), `aero.ini`, turbo lag, ABS and
traction control (`electronics.ini`), the damage model, `.ksanim` animations, sound banks, driver
position, `lights.ini`, and the car's own `collider.ini`.

## Measured

- All 178 installed cars of the base game decrypt and give a full spec (mass, drive, torque curve, gears);
  `MINIENGINE_AC_CARS=<...>/content/cars miniengine_kn5_import_tests` runs that check. Two cars
  (`ks_ferrari_488_challenge_evo`, `ks_ferrari_488_gt3_2020`) ship no `data.acd` and no `data/`.
- The Boxster imports as 1460 kg, RWD, 386 Nm (2000 to 4500 rpm), 7500 rpm, seven gears, 26.7 degree
  lock, 800 Nm brakes per wheel at 65% front, 1.78 Hz springs.
- **Driving it is slower than the default tuning, not faster.** On flat tarmac 0 to 100 km/h takes about
  9 s on its own data against 6.5 s on the defaults (the real car: 4.2 s). Jolt's tyres and clutch launch
  a 386 Nm rear-drive car with a 14:1 first gear badly, and the time swings by seconds with small changes
  (1460 kg instead of 1400 costs 1.7 s). A traction control tried against it (cutting the throttle on rim
  slip) made it worse and was dropped. The test only bounds the time (under 14 s) and checks that the
  gearbox shifts.

## Automated Verification

- `AcdKeysMatchTheGame`: keys of real archives (Boxster, abarth500, bmw_m3_e30, lotus_49, p4-5_2011).
- `AcdArchiveDecryptsAndRefusesAWrongFolder`: round trip for four values of the seventh number, both
  layouts, another case of the folder name; a renamed folder, a truncated archive and garbage throw.
- `CarDataBecomesASpec`: every conversion above on the Boxster's figures; missing files leave fields out.
- `ImportWritesTheCarsOwnData`: no data, a `data.acd`, one for another folder (import survives, problem
  reported), an unpacked `data/`; the figures survive the glTF and the loader.
- `TestCarSpecReplacesWhatItKnows`, `TestCarOnItsOwnDataAccelerates` (physics).

## Manual Acceptance (by drive)

Re-import the Boxster (an import never overwrites: import it under a new name or delete
`assets/models/porsche_boxster_s_pdk`). Drive it: the Vehicle panel shows "Car's own data: 1460 kg, RWD,
386 Nm, 7500 rpm, 7 gears, brakes, springs"; the gear readout goes up to 7 with the revs falling at each
change; the steering wheel turns 400 degrees at full lock; switching "Use the Car's Own Data" off gives
the old car.
