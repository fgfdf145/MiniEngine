#include "suspension_rig_report.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace me::suspension
{

namespace
{
constexpr RigMode kModes[4] = {RigMode::Heave, RigMode::Pitch, RigMode::Roll, RigMode::Warp};

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream file(path);
    if (!file)
    {
        throw std::runtime_error("cannot write " + path.string());
    }
    file << std::setprecision(6);
    return file;
}

void WriteKc(const KcResult& kc, const std::filesystem::path& out)
{
    for (int axle = 0; axle < 2; ++axle)
    {
        const std::string name = axle == 0 ? "front" : "rear";
        std::ofstream bounce = Open(out / ("kc_bounce_" + name + ".csv"));
        bounce << "travel_mm,camber_l_deg,camber_r_deg,toe_l_deg,toe_r_deg,half_track_mm,wheelbase_mm,pad_force_l_n,wheel_rate_n_per_mm,kpi_deg,caster_deg,scrub_mm,trail_mm,roll_center_mm,contact_path_deg,center_path_deg\n";
        for (const KcBouncePoint& p : kc.bounce[axle])
        {
            bounce << p.travel << ',' << p.left.camber << ',' << p.right.camber << ',' << p.left.toe << ',' << p.right.toe << ',' << p.left.halfTrackChange << ','
                   << p.left.wheelbaseChange << ',' << p.left.padForce << ',' << p.wheelRate << ',' << p.kingpinInclination << ',' << p.caster << ','
                   << p.scrubRadius << ',' << p.casterTrail << ',' << p.rollCenterHeight << ',' << p.contactPathAngle << ',' << p.centerPathAngle << '\n';
        }
        std::ofstream roll = Open(out / ("kc_roll_" + name + ".csv"));
        roll << "roll_deg,camber_to_road_l_deg,camber_to_road_r_deg,toe_l_deg,toe_r_deg,pad_force_l_n,pad_force_r_n,roll_moment_nm\n";
        for (const KcRollPoint& p : kc.roll[axle])
        {
            roll << p.roll << ',' << p.left.camber << ',' << p.right.camber << ',' << p.left.toe << ',' << p.right.toe << ',' << p.left.padForce << ','
                 << p.right.padForce << ',' << p.rollMoment << '\n';
        }
    }
    std::ofstream steer = Open(out / "kc_steer.csv");
    steer << "rack_mm,steering_wheel_deg,left_deg,right_deg,ackermann_pct\n";
    for (const KcSteerPoint& p : kc.steer)
    {
        steer << p.rack << ',' << p.steeringWheel << ',' << p.left << ',' << p.right << ',' << p.ackermann << '\n';
    }
}

void WriteSweep(const SweepResult& sweep, const std::filesystem::path& path)
{
    std::ofstream file = Open(path);
    file << "frequency_hz,pad_mm,body_gain,body_phase_deg,body_accel_gain,load_var_fl,load_var_fr,load_var_rl,load_var_rr,wheel_gain_fl,wheel_gain_rl\n";
    for (const SweepCycle& c : sweep.cycles)
    {
        file << c.frequency << ',' << c.padAmplitude * 1000.0 << ',' << c.bodyGain << ',' << c.bodyPhase << ',' << c.bodyAccelGain << ',' << c.loadVariation[0] << ','
             << c.loadVariation[1] << ',' << c.loadVariation[2] << ',' << c.loadVariation[3] << ',' << c.wheelGain[0] << ',' << c.wheelGain[2] << '\n';
    }
}

std::string Value(double v, const char* none)
{
    std::ostringstream text;
    if (v > 0.0)
    {
        text << std::fixed << std::setprecision(2) << v;
    }
    else
    {
        text << none;
    }
    return text.str();
}
}

CarModel WithAssumedFriction(const CarModel& car)
{
    CarModel withFriction = car;
    for (CarCorner& corner : withFriction.corners)
    {
        corner.hasFriction = true;
        corner.friction.stribeck.coulomb = {60.0, 0.0, 0.0};
        corner.friction.stribeck.breakaway = {90.0, 0.0, 0.0};
        corner.friction.stribeck.stribeckVelocity = 0.005;
        corner.friction.bristleStiffness = 90.0 / 1e-4;
        corner.friction.bristleDamping = 2.0 * std::sqrt(corner.friction.bristleStiffness * 10.0);
    }
    return withFriction;
}

std::optional<RigReport> RunRigReport(const CarModel& car, const RigReportOptions& options, const RigReportProgress& progress)
{
    const auto start = std::chrono::steady_clock::now();
    RigReport report;
    report.carName = car.name;
    report.mass = car.mass;
    report.sprungMass = car.sprungMass;
    report.wheelbase = car.wheelbase;
    report.cgHeight = car.cgHeight;
    report.rollInertia = car.rollInertia;
    report.pitchInertia = car.pitchInertia;
    report.options = options;
    report.hasFriction = options.frictionComparison;
    const CarModel withFriction = WithAssumedFriction(car);

    // Rough weights of each test's run time, for the progress.
    const double runs = options.frictionComparison ? 2.0 : 1.0;
    const double total = 0.2 + 4.0 * 2.0 * runs + 0.5 + 0.3 + 0.1 + 2.0 * runs;
    double done = 0.0;
    const auto next = [&](double weight, const char* stage) {
        const bool goOn = !progress || progress(done / total, stage);
        done += weight;
        return goOn;
    };

    if (!next(0.2, "K&C"))
    {
        return std::nullopt;
    }
    report.kc = RunKcRig(car);

    const SineSweep sweep(options.sweepStartHz, options.sweepEndHz, options.sweepCycles);
    for (int m = 0; m < 4; ++m)
    {
        const std::string stage = std::string("seven-post sweep: ") + RigModeName(kModes[m]);
        if (!next(2.0, stage.c_str()))
        {
            return std::nullopt;
        }
        report.sweeps[m] = RunSweep(car, kModes[m], sweep, options.sweepAmplitude, options.sweepMaxVelocity, false);
        if (options.frictionComparison)
        {
            const std::string withStage = stage + " (with friction)";
            if (!next(2.0, withStage.c_str()))
            {
                return std::nullopt;
            }
            report.frictionSweeps[m] = RunSweep(withFriction, kModes[m], sweep, options.sweepAmplitude, options.sweepMaxVelocity, true);
        }
    }

    if (!next(0.5, "heave step"))
    {
        return std::nullopt;
    }
    report.step = RunStep(car, RigMode::Heave, options.stepHeight);
    if (!next(0.3, "aero loaders"))
    {
        return std::nullopt;
    }
    report.aero = RunAeroLoads(car, options.aeroDownforce, options.aeroFrontShare);
    if (!next(0.1, "warp"))
    {
        return std::nullopt;
    }
    report.warp = RunWarp(car, options.warp);

    for (const auto& [name, phi0] : {std::pair<const char*, double>{"smooth track", 1e-7}, std::pair<const char*, double>{"bumpy road", 2e-6}})
    {
        const std::string stage = std::string("random road: ") + name;
        if (!next(runs, stage.c_str()))
        {
            return std::nullopt;
        }
        RigRoadCase road;
        road.name = name;
        road.phi0 = phi0;
        road.plain = RunRoad(car, options.roadSpeed, phi0, 2.0, options.roadSeconds, 7, false);
        if (options.frictionComparison)
        {
            road.friction = RunRoad(withFriction, options.roadSpeed, phi0, 2.0, options.roadSeconds, 7, true);
        }
        report.roads.push_back(road);
    }
    if (progress)
    {
        progress(1.0, "done");
    }
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return report;
}

std::string FormatRigSummary(const RigReport& report)
{
    const RigReportOptions& o = report.options;
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2);
    summary << "# " << report.carName << ": K&C and seven-post\n\n";
    summary << "Mass " << report.mass << " kg (sprung " << report.sprungMass << "), wheelbase " << report.wheelbase << " m, centre of mass " << report.cgHeight
            << " m up, inertia roll " << report.rollInertia << " / pitch " << report.pitchInertia << " kg m^2.\n\n";
    summary << "## K&C\n\n| | front | rear |\n|---|---|---|\n";
    const auto row = [&](const char* name, double front, double rear) {
        summary << "| " << name << " | " << front << " | " << rear << " |\n";
    };
    const KcResult& kc = report.kc;
    const KcAxleSummary& f = kc.axles[0];
    const KcAxleSummary& r = kc.axles[1];
    row("wheel rate (N/mm)", f.wheelRate, r.wheelRate);
    row("bump steer (deg/m, toe-in +)", f.bumpSteer, r.bumpSteer);
    row("camber gain (deg/m, to body)", f.camberGain, r.camberGain);
    row("half-track change (mm/m)", f.trackChange, r.trackChange);
    row("roll centre height (mm)", f.rollCenterHeight, r.rollCenterHeight);
    row("roll stiffness (Nm/deg)", f.rollStiffness, r.rollStiffness);
    row("roll steer (deg/deg, to the outside +)", f.rollSteer, r.rollSteer);
    row("roll camber, outer wheel to road (deg/deg)", f.rollCamber, r.rollCamber);
    row("kingpin inclination (deg)", f.kingpinInclination, r.kingpinInclination);
    row("caster (deg)", f.caster, r.caster);
    row("scrub radius (mm)", f.scrubRadius, r.scrubRadius);
    row("caster trail (mm)", f.casterTrail, r.casterTrail);
    row("contact path angle (deg)", f.contactPathAngle, r.contactPathAngle);
    row("wheel centre path angle (deg)", f.centerPathAngle, r.centerPathAngle);
    summary << "\nRoll stiffness on the front: " << kc.rollStiffnessFrontShare * 100.0 << " %. Steering ratio " << kc.steeringRatio << ":1. Anti-dive front "
            << kc.antiDiveFront << " %, anti-lift rear " << kc.antiLiftRear << " %, anti-squat rear " << kc.antiSquatRear << " %.\n";
    if (!kc.steer.empty())
    {
        summary << "Ackermann at full lock: " << kc.steer.back().ackermann << " % (left " << kc.steer.back().left << " deg, right " << kc.steer.back().right
                << " deg).\n";
    }
    summary << "\n## Seven-post\n\nSine sweep " << std::defaultfloat << o.sweepStartHz << "-" << o.sweepEndHz << " Hz, " << o.sweepCycles + 1 << " cycles, pads "
            << std::fixed << o.sweepAmplitude * 1000.0 << " mm but at most " << o.sweepMaxVelocity << " m/s.\n\n";
    if (report.hasFriction)
    {
        summary << "| mode | body f (Hz) | body peak gain | damping (half power) | wheel hop (Hz) | peak load variation | with friction: f | gain | load variation |\n"
                   "|---|---|---|---|---|---|---|---|---|\n";
    }
    else
    {
        summary << "| mode | body f (Hz) | body peak gain | damping (half power) | wheel hop (Hz) | peak load variation |\n|---|---|---|---|---|---|\n";
    }
    for (int m = 0; m < 4; ++m)
    {
        const SweepResult& plain = report.sweeps[m];
        summary << "| " << RigModeName(kModes[m]) << " | " << Value(plain.bodyFrequency, "-") << " | " << Value(plain.bodyPeakGain, "-") << " | "
                << Value(plain.bodyDamping, "-") << " | " << Value(plain.wheelHopFrequency, "none (over-damped)") << " | " << Value(plain.peakLoadVariation, "-")
                << " |";
        if (report.hasFriction)
        {
            const SweepResult& rough = report.frictionSweeps[m];
            summary << " " << Value(rough.bodyFrequency, "-") << " | " << Value(rough.bodyPeakGain, "-") << " | " << Value(rough.peakLoadVariation, "-") << " |";
        }
        summary << "\n";
    }
    const StepResponse& step = report.step;
    summary << "\nHeave step " << std::defaultfloat << o.stepHeight * 1000.0 << std::fixed << " mm: overshoot " << step.overshoot * 100.0 << " %, damping ratio " << step.dampingRatio
            << ", settles in " << step.settlingTime << " s.\n";
    if (!report.aero.empty())
    {
        summary << "Aero loaders (" << std::defaultfloat << o.aeroFrontShare * 100.0 << std::fixed << " % front): " << report.aero.back().downforce << " N lowers the front "
                << -report.aero.back().frontHeight << " mm and the rear " << -report.aero.back().rearHeight << " mm.\n";
    }
    summary << "Warp " << std::defaultfloat << o.warp * 1000.0 << std::fixed << " mm at the pads: diagonal load transfer " << report.warp.diagonalTransfer << " N (" << report.warp.warpStiffness
            << " N/mm).\n\n";
    summary << "Random road at " << std::defaultfloat << o.roadSpeed << " m/s, " << o.roadSeconds << std::fixed << " s (left and right independent, rear following front):\n\n";
    if (report.hasFriction)
    {
        summary << "| road | load RMS / static, FL / RL | body accel RMS (m/s^2) | travel RMS FL (mm) | lift-off (s) | with friction: load RMS FL / RL | body accel |\n"
                   "|---|---|---|---|---|---|---|\n";
    }
    else
    {
        summary << "| road | load RMS / static, FL / RL | body accel RMS (m/s^2) | travel RMS FL (mm) | lift-off (s) |\n|---|---|---|---|---|\n";
    }
    for (const RigRoadCase& road : report.roads)
    {
        summary << "| " << road.name << " (phi0 " << std::defaultfloat << road.phi0 << " m^3) | " << std::fixed << std::setprecision(3) << road.plain.loadRms[0]
                << " / " << road.plain.loadRms[2] << " | " << road.plain.bodyAccelRms << " | " << road.plain.travelRms * 1000.0 << " | "
                << road.plain.liftOffSeconds << " |";
        if (report.hasFriction)
        {
            summary << " " << road.friction.loadRms[0] << " / " << road.friction.loadRms[2] << " | " << road.friction.bodyAccelRms << " |";
        }
        summary << "\n";
    }
    summary << std::setprecision(2) << "\nRun took " << report.seconds << " s.\n";
    return summary.str();
}

void WriteRigReport(const RigReport& report, const std::filesystem::path& folder)
{
    std::filesystem::create_directories(folder);
    WriteKc(report.kc, folder);
    for (int m = 0; m < 4; ++m)
    {
        WriteSweep(report.sweeps[m], folder / (std::string("sweep_") + RigModeName(kModes[m]) + ".csv"));
        if (report.hasFriction)
        {
            WriteSweep(report.frictionSweeps[m], folder / (std::string("sweep_") + RigModeName(kModes[m]) + "_friction.csv"));
        }
    }
    {
        std::ofstream file = Open(folder / "step_heave.csv");
        file << "time_s,body_mm,fl_load_n\n";
        for (std::size_t i = 0; i < report.step.time.size(); i += 2)
        {
            file << report.step.time[i] << ',' << report.step.body[i] * 1000.0 << ',' << report.step.frontLoad[i] << '\n';
        }
    }
    {
        std::ofstream file = Open(folder / "aero.csv");
        file << "downforce_n,front_mm,rear_mm\n";
        for (const AeroPoint& a : report.aero)
        {
            file << a.downforce << ',' << a.frontHeight << ',' << a.rearHeight << '\n';
        }
    }
    std::ofstream summary = Open(folder / "summary.md");
    summary << FormatRigSummary(report);
}
}
