#pragma once

#include "flex_ring_modal.h"
#include "flex_ring_tyre.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace me::tyre::flexring
{

// Virtual test rigs for a flexible ring tyre (the measurements FTire is parameterized and validated with:
// FTire documentation 5.2, Gipser 2006 "Experiences in parameterization", Gipser 1999 figs. 5-12).
//
// Axes: the road is z = 0 (or the given road), the wheel heads +x, its spin axis is +y (ISO: x forward, y
// left, z up); camber turns the spin axis about +x. Forces and moments are the tyre's on the rim, about
// the rim centre, in road axes (x heading, y lateral, z up). Static and steady points also give the wheel
// load: the road's vertical force (the rim's plus the belt's weight).

struct RigPose
{
    double x = 0.0;
    double y = 0.0;
    double heading = 0.0; // rad about +z
    double camber = 0.0;  // rad about the heading axis
};

// The rim at a pose, centre height, turned `spinAngle` about its axis, with a velocity and a spin rate.
RimState MakeRim(const RigPose& pose, double centreHeight, double spinAngle, const Vec3& velocity, double spinRate);

// The lowest point of the unloaded tread at a pose (centre at height 0): the rim height minus it is where
// the tyre first touches a flat road.
double RestTouchHeight(FlexRingTyre& tyre, const RigPose& pose);

// Forces in road axes (heading, lateral, up) from a rim wrench.
struct RoadForces
{
    double fx = 0.0, fy = 0.0, fz = 0.0;
    double mx = 0.0, my = 0.0, mz = 0.0;
};
RoadForces ToRoadAxes(const Wrench& wrench, double heading);

struct StaticPoint
{
    double deflection = 0.0; // m
    RoadForces forces;
    ContactStats contact;
    bool converged = false;
};

// Loads the tyre quasi-statically on the road through the deflections (m, from first touch, increasing),
// in sub-steps of at most `increment`; the tyre keeps the last state.
std::vector<StaticPoint> StaticDeflection(FlexRingTyre& tyre, const Road& road, const RigPose& pose, const std::vector<double>& deflections, double increment = 0.002);

// The deflection (m) that carries `load` (N) on the road, by secant steps on StaticDeflection from the
// unloaded tyre; the tyre is left there.
double DeflectionForLoad(FlexRingTyre& tyre, const Road& road, const RigPose& pose, double load, double stiffnessGuess, double tolerance = 2.0e-4);

// Static stiffnesses of the loaded tyre (FTire 6.4: tire_long_stiffn, tire_lat_stiffn, tire_tors_stiffn):
// the rim moved by small steps along x, y and turned about z with the tread stuck to the road.
struct StaticStiffness
{
    double vertical = 0.0;     // N/m, secant at the deflection
    double longitudinal = 0.0; // N/m
    double lateral = 0.0;      // N/m
    double torsional = 0.0;    // N m/rad
    double load = 0.0;
};
StaticStiffness MeasureStaticStiffness(const FlexRingParameters& parameters, double deflection);

// A tyre rolling on a flat road at constant speed, slip angle, camber and slip ratio (or free rolling),
// at constant load (the rim height regulated) until steady.
struct SteadySettings
{
    double speed = 60.0 / 3.6; // m/s
    double load = 4000.0;      // N
    double slipAngle = 0.0;    // rad, ISO: travel to the right of the heading is positive
    double slipRatio = 0.0;    // (omega Re - V) / V, positive driving
    double camber = 0.0;       // rad
    bool freeRolling = true;   // the wheel spins freely (slip ratio ignored)
    double effectiveRadius = 0.0; // m; 0: measured by free rolling first
    double settleTime = 0.30;  // s
    double averageTime = 0.05; // s
    double dt = 0.001;         // s, rim update interval
    double roadFriction = 1.0;
};
struct SteadyPoint
{
    SteadySettings settings;
    RoadForces forces;
    ContactStats contact;
    double spinRate = 0.0;        // rad/s
    double effectiveRadius = 0.0; // V / omega
    double deflection = 0.0;      // rim height below touch, m
    double cpuSeconds = 0.0;
};
SteadyPoint RunSteady(const FlexRingParameters& parameters, const SteadySettings& settings, FlexRingTyre* warmStart = nullptr);

// Sweeps of slip angle, slip ratio or camber at one load (sequential, each point starting from the last).
enum class SweepKind
{
    SlipAngle,
    SlipRatio,
    Camber,
};
std::vector<SteadyPoint> RunSweep(const FlexRingParameters& parameters, SweepKind kind, const std::vector<double>& values, SteadySettings base,
                                  const std::atomic<bool>* cancel = nullptr);

// Rolling over a cleat with the spindle at a fixed height (FTire's cleat test, Gipser 1999 figs. 5, 6):
// the spindle height carries `load` on the flat road, the wheel turns freely.
struct CleatSettings
{
    double speed = 40.0 / 3.6;
    double load = 4000.0;
    CleatGeometry cleat;
    double runUp = 0.6;  // m of flat road before the cleat
    double runOut = 0.4; // m after it
    double dt = 0.0005;  // s, output interval
};
struct CleatSample
{
    double time = 0.0;
    double position = 0.0; // rim x relative to the cleat
    RoadForces forces;
    double spinRate = 0.0;
};
std::vector<CleatSample> RunCleat(const FlexRingParameters& parameters, const CleatSettings& settings, const std::atomic<bool>* cancel = nullptr);

// Wall time of the model: a tyre rolling at 60 km/h, 4 kN, 2 deg slip angle on a flat road, after settling.
struct BenchmarkResult
{
    int segments = 0;
    int blocksPerSegment = 0;
    double maxStep = 0.0;
    double maxAngle = 0.0;
    double secondsPerSimulatedSecond = 0.0; // real-time factor of one tyre on one core
    double microsecondsPerStep = 0.0;       // per internal step
    double stepsPerSecond = 0.0;            // internal steps per simulated second
    int blocksInContact = 0;
    double parallelFourTyres = 0.0; // real-time factor of four tyres, one thread each
};
BenchmarkResult RunBenchmark(const FlexRingParameters& parameters, double simulatedSeconds = 0.5, bool fourInParallel = true);

// Runs `body(i)` for i in [0, count) on up to `threads` threads (0: all cores).
void ParallelFor(int count, const std::function<void(int)>& body, int threads = 0);

}
