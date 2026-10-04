#pragma once

#include "suspension_axle.h"
#include "suspension_corner.h"
#include "suspension_friction.h"
#include "suspension_model.h"
#include "suspension_strut.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace me::suspension
{

// ---- A whole car for the rigs ----
//
// Car frame: x forward, y left, z up, origin at the sprung mass's centre at road level. Corners
// FL, FR, RL, RR. Each corner's definition is in its own frame (from its wheel centre, as
// SuspensionCorner takes it); `position` places that wheel centre in the car frame.
struct CarCorner
{
    SuspensionDefinition definition;
    // The force unit and where it acts: on an element of the definition, or straight on the
    // wheel travel (SuspensionCorner::kWheelTravel) when its rates are at the wheel.
    StrutUnitSettings unit;
    int unitElement = SuspensionCorner::kWheelTravel;
    int slider = -1;
    bool hasFriction = false;
    LuGreParameters friction;
    double antiRollBarRate = 0.0; // N/m of travel difference with the other wheel of the axle
    double hubMass = 0.0;         // unsprung, kg
    double tyreRate = 0.0;        // N/m
    double tyreDamping = 0.0;     // N s/m
    Vec3 position{0.0};
};

struct CarModel
{
    std::string name;
    std::array<CarCorner, 4> corners;
    // An axle (front, rear) that is a solid axle: its corners' definitions are then unused, their
    // units are its springs and dampers (rates at the units, at springPosition along the axle).
    std::array<std::optional<SolidAxleDefinition>, 2> solidAxles;
    double mass = 0.0;        // the whole car, kg
    double sprungMass = 0.0;
    double rollInertia = 0.0; // of the sprung mass about its centre, kg m^2
    double pitchInertia = 0.0;
    double cgHeight = 0.0;    // the sprung mass's centre above the road, m
    double wheelbase = 0.0;
    double frontBrakeShare = 0.6;
    // Rack travel at full lock (signed so that positive steers right) and the steering wheel's
    // angle there, for the steering ratio.
    double rackAtLock = 0.0;
    double steeringWheelLockDegrees = 0.0;
    // Each corner's static sprung load (N), the springs' preload.
    std::array<double, 4> staticLoad{};
};

// Fills staticLoad and each unit's preload so the car rests at its design position, and moves the
// car frame's origin onto the sprung mass's centre (from the corners' loads).
void BalanceCar(CarModel& car, double frontAxleShareOfWeight);

StrutUnit MakeCornerUnit(const CarCorner& corner);

// The front (0) or rear (1) axle's two wheels for a rig: their corners, or their solid axle.
std::unique_ptr<AxleSuspension> MakeAxleSuspension(const CarModel& car, int axle, bool friction = true);

// ---- K&C rig: quasi-static, body held, wheel pads at set heights ----

struct KcWheel
{
    double camber = 0.0;    // deg, top outward +, relative to the body
    double toe = 0.0;       // deg, toe-in +
    double halfTrackChange = 0.0; // mm
    double wheelbaseChange = 0.0; // mm
    double padForce = 0.0;  // N, the road's vertical push (spring, stops, anti-roll bar)
};

struct KcBouncePoint
{
    double travel = 0.0; // mm, both wheels of the axle
    KcWheel left;
    KcWheel right;
    double kingpinInclination = 0.0; // deg (left wheel)
    double caster = 0.0;
    double scrubRadius = 0.0;        // mm
    double casterTrail = 0.0;        // mm
    double rollCenterHeight = 0.0;   // mm above the road
    double contactPathAngle = 0.0;   // deg, forward per upward of the contact point
    double centerPathAngle = 0.0;    // deg, of the wheel centre
    double wheelRate = 0.0;          // N/mm per wheel, from the pad forces
};

struct KcRollPoint
{
    double roll = 0.0; // deg, body rolled to the right (right side down: a left-hand turn)
    KcWheel left;      // camber here relative to the road
    KcWheel right;
    double rollMoment = 0.0; // Nm the pads take about the axle's centre
};

struct KcSteerPoint
{
    double rack = 0.0;            // mm
    double steeringWheel = 0.0;   // deg
    double left = 0.0;            // deg the wheel turned right (positive) from straight
    double right = 0.0;
    double ackermann = 0.0;       // %: (inner - outer) / (ideal inner - outer), turns over 1 deg
};

struct KcAxleSummary
{
    double bumpSteer = 0.0;        // deg/m, toe-in per metre of bump (left wheel)
    double camberGain = 0.0;       // deg/m, relative to the body
    double trackChange = 0.0;      // mm/m half-track per metre of bump
    double wheelRate = 0.0;        // N/mm
    double rollStiffness = 0.0;    // Nm/deg, springs and anti-roll bar
    double rollSteer = 0.0;        // deg of axle steer towards the outside of the turn per deg of roll
    double rollCamber = 0.0;       // deg of outer-wheel camber to the road per deg of roll
    double rollCenterHeight = 0.0; // mm
    double kingpinInclination = 0.0;
    double caster = 0.0;
    double scrubRadius = 0.0;      // mm
    double casterTrail = 0.0;      // mm
    double contactPathAngle = 0.0; // deg
    double centerPathAngle = 0.0;  // deg
};

struct KcResult
{
    std::array<std::vector<KcBouncePoint>, 2> bounce; // front, rear
    std::array<std::vector<KcRollPoint>, 2> roll;
    std::vector<KcSteerPoint> steer;
    std::array<KcAxleSummary, 2> axles;
    double rollStiffnessFrontShare = 0.0; // of the car's
    double steeringRatio = 0.0;           // steering wheel deg per mean front wheel deg near centre
    double antiDiveFront = 0.0;           // %, outboard front brakes
    double antiLiftRear = 0.0;            // %, braking, outboard rear brakes
    double antiSquatRear = 0.0;           // %, rear drive, inboard differential
};

KcResult RunKcRig(const CarModel& car, double bounceRange = 0.06, double rollRange = 3.0, double rackFraction = 1.0);

// ---- Seven-post rig: four wheel pads and three body loaders ----

enum class RigMode
{
    Heave,
    Pitch,
    Roll,
    Warp,
};

const char* RigModeName(RigMode mode);
// Each pad's share of the input in a mode (FL, FR, RL, RR): +1 or -1.
std::array<double, 4> RigModePattern(RigMode mode);

// How the rig steps the unsprung masses.
enum class UnsprungScheme
{
    // Semi-implicit Euler for body and hubs together: the reference at a small step.
    SemiImplicit,
    // The game's (PhysicsWorld::StepUnsprungCorner): each hub's travel by the trapezoidal rule on the
    // force's slopes, with the tyre, anti-roll bar and the body's acceleration frozen at the step's
    // start; the body (sprung mass and hubs as one, as the physics engine has it) then takes the
    // tyres' force and the hubs' relative inertia. For measuring that scheme's error at the game's step.
    GameLinearlyImplicit,
};

// The car on the rig: sprung body in heave, pitch and roll; four unsprung masses; tyres as
// springs (they leave the pad when unloaded); each corner's suspension through SuspensionCorner.
// Semi-implicit Euler at the step it is given, or the game's scheme.
class SevenPostRig
{
public:
    explicit SevenPostRig(const CarModel& car, bool friction = true, UnsprungScheme scheme = UnsprungScheme::SemiImplicit);

    // Pads' heights and rates (m, m/s) and the loaders' heave force (N, up), pitch moment (Nm, nose
    // up) and roll moment (Nm, right side down).
    void Step(const std::array<double, 4>& pads, const std::array<double, 4>& padRates, double heaveForce, double pitchMoment, double rollMoment, double dt);

    double Heave() const
    {
        return m_heave;
    }
    double Pitch() const
    {
        return m_pitch; // rad, nose up
    }
    double Roll() const
    {
        return m_roll; // rad, right side down
    }
    double HeaveAcceleration() const
    {
        return m_heaveAccel;
    }
    double PitchAcceleration() const
    {
        return m_pitchAccel;
    }
    double RollAcceleration() const
    {
        return m_rollAccel;
    }
    double Travel(int corner) const
    {
        return m_travel[corner];
    }
    double WheelHeight(int corner) const
    {
        return m_wheel[corner];
    }
    // The tyre's load on the pad, N (static included).
    double TyreLoad(int corner) const
    {
        return m_tyreLoad[corner];
    }
    double StaticTyreLoad(int corner) const
    {
        return m_staticTyreLoad[corner];
    }
    const CarModel& Car() const
    {
        return m_car;
    }
    const AxleSuspension& Axle(int axle) const
    {
        return *m_axles[axle];
    }

private:
    void StepGameScheme(const std::array<double, 4>& pads, const std::array<double, 4>& padRates, double heaveForce, double pitchMoment, double rollMoment, double dt,
                        const std::array<double, 4>& travel, const std::array<double, 4>& travelRate);

    CarModel m_car;
    UnsprungScheme m_scheme = UnsprungScheme::SemiImplicit;
    std::array<std::unique_ptr<AxleSuspension>, 2> m_axles;
    std::array<double, 4> m_staticTyreLoad{};
    double m_heave = 0.0;
    double m_pitch = 0.0;
    double m_roll = 0.0;
    double m_heaveRate = 0.0;
    double m_pitchRate = 0.0;
    double m_rollRate = 0.0;
    double m_heaveAccel = 0.0;
    double m_pitchAccel = 0.0;
    double m_rollAccel = 0.0;
    std::array<double, 4> m_wheel{};
    std::array<double, 4> m_wheelRate{};
    std::array<double, 4> m_travel{};
    std::array<double, 4> m_travelPrevious{};
    std::array<double, 4> m_tyreLoad{};
};

// Rill's sine sweep with periods shrinking linearly (Road Vehicle Dynamics, eq. 6.56):
// x(t) = sin(2 pi / q * ln(p / (p - q t))), N + 1 cycles from f0 to fE.
struct SineSweep
{
    double f0 = 0.5;
    double fE = 25.0;
    int cycles = 100;
    double p = 0.0;
    double q = 0.0;

    SineSweep(double startHz, double endHz, int cycleCount);
    double Duration() const;
    double Phase(double t) const;     // 2 pi h(t)
    double Frequency(double t) const; // dh/dt
    double CycleStart(int n) const;   // t_n
};

struct SweepCycle
{
    double frequency = 0.0;     // Hz, 1 / the cycle's period
    double padAmplitude = 0.0;  // m
    double bodyGain = 0.0;      // the mode's body displacement over the pad's (first harmonic)
    double bodyPhase = 0.0;     // deg
    double bodyAccelGain = 0.0; // body acceleration over pad acceleration
    std::array<double, 4> loadVariation{}; // first-harmonic tyre load amplitude / static load
    std::array<double, 4> wheelGain{};     // unsprung displacement over the pad's
    std::array<double, 4> loadRms{};       // RMS of (load - static) / static over the cycle
};

struct SweepResult
{
    RigMode mode = RigMode::Heave;
    std::vector<SweepCycle> cycles;
    double bodyFrequency = 0.0;      // Hz where bodyGain peaks
    double bodyPeakGain = 0.0;
    double bodyDamping = 0.0;        // half-power estimate of the body peak's damping ratio (0 when not resolved)
    double wheelHopFrequency = 0.0;  // Hz of the wheels' highest local gain peak above 6 Hz (0: none)
    double peakLoadVariation = 0.0;  // the largest loadVariation of any wheel
};

// Sine sweep in one mode: the pads move by `amplitude` (m) but no faster than `maxVelocity` (m/s).
SweepResult RunSweep(const CarModel& car, RigMode mode, const SineSweep& sweep, double amplitude, double maxVelocity, bool friction = true, double dt = 1e-3,
                     UnsprungScheme scheme = UnsprungScheme::SemiImplicit);

struct StepResponse
{
    std::vector<double> time;
    std::vector<double> body;       // the mode's body displacement, m
    std::vector<double> frontLoad;  // FL tyre load, N
    double overshoot = 0.0;         // of the final value, fraction
    double dampingRatio = 0.0;      // from the logarithmic decrement (1 when it does not overshoot)
    double settlingTime = 0.0;      // s, to within 2%
};

StepResponse RunStep(const CarModel& car, RigMode mode, double height, double seconds = 2.0, bool friction = true, double dt = 1e-3,
                     UnsprungScheme scheme = UnsprungScheme::SemiImplicit);

struct AeroPoint
{
    double downforce = 0.0;   // N
    double frontHeight = 0.0; // mm change of the body at the front axle
    double rearHeight = 0.0;
};

// Body loaders press with `downforce` steps up to the maximum, `frontShare` of it on the front axle.
std::vector<AeroPoint> RunAeroLoads(const CarModel& car, double maxDownforce, double frontShare, int steps = 8, bool friction = true);

struct WarpResult
{
    double padWarp = 0.0;          // m: FL and RR up, FR and RL down by this
    double diagonalTransfer = 0.0; // N: (FL + RR - FR - RL) / 2 change
    double warpStiffness = 0.0;    // N/mm of pad warp
};

WarpResult RunWarp(const CarModel& car, double padWarp, bool friction = true);

// A random road (Rill eq. 2.x: PSD Phi(Omega) = phi0 (Omega / 1)^-w over the sampled wavelengths),
// driven at `speed`: left and right tracks independent, the rear following the front by the
// wheelbase. The statistics leave out the first 2 s (the road fades in over the first): `seconds`
// must be longer (std::invalid_argument otherwise).
// The road itself: two tracks (0 left, 1 right) of Rill's sum of sines, heights in m against the
// distance along the road in m.
class RandomRoad
{
public:
    RandomRoad(double phi0, double waviness, std::uint32_t seed);
    double Height(int track, double distance) const;

private:
    std::vector<double> m_omega;
    std::vector<double> m_amplitude;
    std::array<std::vector<double>, 2> m_phase;
};

struct RoadResult
{
    std::array<double, 4> loadRms{};   // RMS of (load - static) / static
    double bodyAccelRms = 0.0;         // m/s^2, heave
    double travelRms = 0.0;            // m, front left
    double liftOffSeconds = 0.0;       // time any tyre was off its pad
};

RoadResult RunRoad(const CarModel& car, double speed, double phi0, double waviness, double seconds, std::uint32_t seed, bool friction = true, double dt = 1e-3,
                   UnsprungScheme scheme = UnsprungScheme::SemiImplicit);
}
