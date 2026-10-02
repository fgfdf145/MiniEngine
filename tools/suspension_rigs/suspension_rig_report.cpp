// Runs a car's suspension through the virtual K&C rig and seven-post rig and writes what they
// measured: CSV files for plotting and summary.md.
//
//   miniengine_suspension_rig_report <Assetto Corsa car folder> <output folder>
//
// The car is read from its data.acd (or data/ folder) as the kn5 import reads it.

#include <engine/asset/ac_car_data.h>
#include <engine/physics/vehicle_suspension.h>
#include <engine/suspension/suspension_rigs.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace me;
using namespace me::suspension;

namespace
{
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
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: miniengine_suspension_rig_report <Assetto Corsa car folder> <output folder>\n";
        return 2;
    }
    try
    {
        const std::filesystem::path carFolder = argv[1];
        const std::filesystem::path out = argv[2];
        std::filesystem::create_directories(out);
        std::string problem;
        const std::optional<VehicleCarSpec> spec = AcCarData::ReadCarFolder(carFolder, &problem);
        if (!spec.has_value())
        {
            throw std::runtime_error("cannot read the car's data: " + problem);
        }
        const auto start = std::chrono::steady_clock::now();
        const CarModel car = BuildCarModel(*spec, carFolder.filename().string());

        // The car's friction is not in its data: a second run with an assumed seal friction at
        // the wheel shows what it does (Coulomb 60 N, breakaway 90 N: an assumption).
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

        const KcResult kc = RunKcRig(car);
        WriteKc(kc, out);

        const SineSweep sweep(0.5, 25.0, 100);
        const double amplitude = 0.003;  // m
        const double maxVelocity = 0.1;  // m/s
        std::ostringstream modes;
        for (RigMode mode : {RigMode::Heave, RigMode::Pitch, RigMode::Roll, RigMode::Warp})
        {
            const SweepResult plain = RunSweep(car, mode, sweep, amplitude, maxVelocity, false);
            const SweepResult rough = RunSweep(withFriction, mode, sweep, amplitude, maxVelocity, true);
            WriteSweep(plain, out / (std::string("sweep_") + RigModeName(mode) + ".csv"));
            WriteSweep(rough, out / (std::string("sweep_") + RigModeName(mode) + "_friction.csv"));
            const auto value = [](double v, const char* none) {
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
            };
            modes << "| " << RigModeName(mode) << " | " << value(plain.bodyFrequency, "-") << " | " << value(plain.bodyPeakGain, "-") << " | "
                  << value(plain.bodyDamping, "-") << " | " << value(plain.wheelHopFrequency, "none (over-damped)") << " | "
                  << value(plain.peakLoadVariation, "-") << " | " << value(rough.bodyFrequency, "-") << " | " << value(rough.bodyPeakGain, "-") << " | "
                  << value(rough.peakLoadVariation, "-") << " |\n";
        }

        const StepResponse step = RunStep(car, RigMode::Heave, 0.01);
        {
            std::ofstream file = Open(out / "step_heave.csv");
            file << "time_s,body_mm,fl_load_n\n";
            for (std::size_t i = 0; i < step.time.size(); i += 2)
            {
                file << step.time[i] << ',' << step.body[i] * 1000.0 << ',' << step.frontLoad[i] << '\n';
            }
        }
        const std::vector<AeroPoint> aero = RunAeroLoads(car, 6000.0, 0.45);
        {
            std::ofstream file = Open(out / "aero.csv");
            file << "downforce_n,front_mm,rear_mm\n";
            for (const AeroPoint& a : aero)
            {
                file << a.downforce << ',' << a.frontHeight << ',' << a.rearHeight << '\n';
            }
        }
        const WarpResult warp = RunWarp(car, 0.01);
        struct RoadCase
        {
            const char* name;
            double phi0;
        };
        std::ostringstream roads;
        for (const RoadCase road : {RoadCase{"smooth track", 1e-7}, RoadCase{"bumpy road", 2e-6}})
        {
            const RoadResult plain = RunRoad(car, 50.0, road.phi0, 2.0, 12.0, 7, false);
            const RoadResult rough = RunRoad(withFriction, 50.0, road.phi0, 2.0, 12.0, 7, true);
            roads << "| " << road.name << " (phi0 " << std::defaultfloat << road.phi0 << " m^3) | " << std::fixed << std::setprecision(3) << plain.loadRms[0] << " / " << plain.loadRms[2] << " | "
                  << plain.bodyAccelRms << " | " << plain.travelRms * 1000.0 << " | " << plain.liftOffSeconds << " | " << rough.loadRms[0] << " / " << rough.loadRms[2]
                  << " | " << rough.bodyAccelRms << " |\n";
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        std::ofstream summary = Open(out / "summary.md");
        summary << std::fixed << std::setprecision(2);
        summary << "# " << car.name << ": K&C and seven-post\n\n";
        summary << "Mass " << car.mass << " kg (sprung " << car.sprungMass << "), wheelbase " << car.wheelbase << " m, centre of mass " << car.cgHeight
                << " m up, inertia roll " << car.rollInertia << " / pitch " << car.pitchInertia << " kg m^2.\n\n";
        summary << "## K&C\n\n| | front | rear |\n|---|---|---|\n";
        const auto row = [&](const char* name, double front, double rear) {
            summary << "| " << name << " | " << front << " | " << rear << " |\n";
        };
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
        summary << "\nRoll stiffness on the front: " << kc.rollStiffnessFrontShare * 100.0 << " %. Steering ratio " << kc.steeringRatio
                << ":1. Anti-dive front " << kc.antiDiveFront << " %, anti-lift rear " << kc.antiLiftRear << " %, anti-squat rear " << kc.antiSquatRear << " %.\n";
        summary << "Ackermann at full lock: " << kc.steer.back().ackermann << " % (left " << kc.steer.back().left << " deg, right " << kc.steer.back().right << " deg).\n\n";
        summary << "## Seven-post\n\nSine sweep 0.5-25 Hz, 101 cycles, pads " << amplitude * 1000.0 << " mm but at most " << maxVelocity << " m/s.\n\n";
        summary << "| mode | body f (Hz) | body peak gain | damping (half power) | wheel hop (Hz) | peak load variation | with friction: f | gain | load variation |\n|---|---|---|---|---|---|---|---|---|\n";
        summary << modes.str();
        summary << "\nHeave step 10 mm: overshoot " << step.overshoot * 100.0 << " %, damping ratio " << step.dampingRatio << ", settles in " << step.settlingTime << " s.\n";
        summary << "Aero loaders (45 % front): " << aero.back().downforce << " N lowers the front " << -aero.back().frontHeight << " mm and the rear "
                << -aero.back().rearHeight << " mm.\n";
        summary << "Warp 10 mm at the pads: diagonal load transfer " << warp.diagonalTransfer << " N (" << warp.warpStiffness << " N/mm).\n\n";
        summary << "Random road at 50 m/s, 12 s (left and right independent, rear following front):\n\n";
        summary << "| road | load RMS / static, FL / RL | body accel RMS (m/s^2) | travel RMS FL (mm) | lift-off (s) | with friction: load RMS FL / RL | body accel |\n|---|---|---|---|---|---|---|\n";
        summary << roads.str();
        summary << "\nRun took " << seconds << " s.\n";
        std::cout << "wrote " << out.string() << " in " << seconds << " s\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "suspension rig report failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
