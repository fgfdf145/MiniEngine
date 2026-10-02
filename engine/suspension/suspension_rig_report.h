#pragma once

#include "suspension_rigs.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace me::suspension
{

// The whole set of rig tests on one car, as miniengine_suspension_rig_report and the editor's
// Suspension Rigs window run it: K&C, a seven-post sine sweep in each mode, a heave step, the aero
// loaders, warp and a random road, each plain and (optionally) again with an assumed damper friction.
struct RigReportOptions
{
    // Repeat the seven-post tests with WithAssumedFriction: the data has no damper friction.
    bool frictionComparison = true;
    double sweepStartHz = 0.5;
    double sweepEndHz = 25.0;
    int sweepCycles = 100;
    double sweepAmplitude = 0.003;  // m at the pads
    double sweepMaxVelocity = 0.1;  // m/s at the pads
    double stepHeight = 0.01;       // m
    double aeroDownforce = 6000.0;  // N
    double aeroFrontShare = 0.45;
    double warp = 0.01;             // m
    double roadSpeed = 50.0;        // m/s
    double roadSeconds = 12.0;
};

struct RigRoadCase
{
    std::string name;
    double phi0 = 0.0;   // m^3
    RoadResult plain;
    RoadResult friction; // when the report has the friction comparison
};

struct RigReport
{
    std::string carName;
    double mass = 0.0;
    double sprungMass = 0.0;
    double wheelbase = 0.0;
    double cgHeight = 0.0;
    double rollInertia = 0.0;
    double pitchInertia = 0.0;
    RigReportOptions options;

    KcResult kc;
    // Heave, pitch, roll, warp (RigMode order).
    std::array<SweepResult, 4> sweeps;
    bool hasFriction = false;
    std::array<SweepResult, 4> frictionSweeps;
    StepResponse step;
    std::vector<AeroPoint> aero;
    WarpResult warp;
    std::vector<RigRoadCase> roads;
    double seconds = 0.0; // wall time the run took
};

// The car with an assumed seal friction at every wheel (Coulomb 60 N, breakaway 90 N): no car data
// gives one, so this only shows what a plausible friction does.
CarModel WithAssumedFriction(const CarModel& car);

// Called between the tests with the share done (0 to 1) and the test about to run; returning false
// stops the run.
using RigReportProgress = std::function<bool(double done, const char* stage)>;

// Runs every test. std::nullopt when `progress` stopped it.
std::optional<RigReport> RunRigReport(const CarModel& car, const RigReportOptions& options = {}, const RigReportProgress& progress = {});

// summary.md's text.
std::string FormatRigSummary(const RigReport& report);

// summary.md and the CSV files (kc_*.csv, sweep_*.csv, step_heave.csv, aero.csv) into `folder`.
// Throws std::runtime_error when a file cannot be written.
void WriteRigReport(const RigReport& report, const std::filesystem::path& folder);
}
