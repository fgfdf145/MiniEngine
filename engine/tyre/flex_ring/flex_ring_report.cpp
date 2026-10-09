#include "flex_ring_report.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <numbers>
#include <sstream>

namespace me::tyre::flexring
{

namespace
{
constexpr double kDeg = std::numbers::pi / 180.0;

std::string Fixed(double v, int digits = 4)
{
    std::ostringstream s;
    s << std::setprecision(digits) << std::fixed << v;
    return s.str();
}

std::string General(double v)
{
    std::ostringstream s;
    s << std::setprecision(8) << v;
    return s.str();
}

class Csv
{
  public:
    Csv(const std::string& directory, const std::string& name, const std::vector<std::string>& header)
    {
        if (directory.empty())
        {
            return;
        }
        m_out = std::make_unique<std::ofstream>(std::filesystem::path(directory) / name);
        for (size_t i = 0; i < header.size(); ++i)
        {
            *m_out << (i ? "," : "") << header[i];
        }
        *m_out << "\n";
    }
    void Row(const std::vector<double>& values, const std::string& prefix = {})
    {
        if (!m_out)
        {
            return;
        }
        bool first = true;
        if (!prefix.empty())
        {
            *m_out << prefix;
            first = false;
        }
        for (double v : values)
        {
            *m_out << (first ? "" : ",") << General(v);
            first = false;
        }
        *m_out << "\n";
    }

  private:
    std::unique_ptr<std::ofstream> m_out;
};

std::vector<double> Range(double from, double to, int count)
{
    std::vector<double> v;
    for (int i = 0; i < count; ++i)
    {
        v.push_back(from + (to - from) * i / std::max(count - 1, 1));
    }
    return v;
}
}

std::vector<ParameterRow> DescribeParameters(const FlexRingParameters& p)
{
    const double chord3 = p.chord * p.chord * p.chord;
    return {
        {"segments", static_cast<double>(p.segments), "-"},
        {"blocks per segment", static_cast<double>(p.blocks.size()), "-"},
        {"belt radius", p.beltRadius, "m"},
        {"outer radius", p.outerRadius, "m"},
        {"rim radius", p.rimRadius, "m"},
        {"belt width", p.beltWidth, "m"},
        {"tread width", p.treadWidth, "m"},
        {"belt lateral curvature radius", p.latCurvatureRadius, "m"},
        {"chord", p.chord, "m"},
        {"node mass", p.nodeMass, "kg"},
        {"belt mass", p.nodeMass * p.segments, "kg"},
        {"rim-fixed tyre mass", p.rimFixedMass, "kg"},
        {"node torsion inertia", p.nodeInertia, "kg m^2"},
        {"radial stiffness per node", p.radialStiffness, "N/m"},
        {"radial progressivity", p.radialProgressivity, "-"},
        {"radial progression scale", p.radialProgressionScale, "m"},
        {"tangential stiffness per node", p.tangentialStiffness, "N/m"},
        {"lateral stiffness per node", p.lateralStiffness, "N/m"},
        {"radial damping per node", p.radialDamping, "N s/m"},
        {"tangential damping per node", p.tangentialDamping, "N s/m"},
        {"lateral damping per node", p.lateralDamping, "N s/m"},
        {"Maxwell radial stiffness", p.maxwellStiffness[0], "N/m"},
        {"Maxwell time", p.maxwellTime, "s"},
        {"torsion stiffness per node", p.torsionStiffness, "N m/rad"},
        {"torsion damping per node", p.torsionDamping, "N m s/rad"},
        {"twist stiffness", p.twistStiffness, "N m/rad"},
        {"belt extension stiffness", p.chordStiffness, "N/m"},
        {"belt extension damping", p.chordDamping, "N s/m"},
        {"in-plane bending EI", p.bendInStiffness * chord3, "N m^2"},
        {"out-of-plane bending EI", p.bendOutStiffness * chord3, "N m^2"},
        {"lateral bending EI per segment", p.lateralBendStiffness, "N m^2"},
        {"pressure force per node", p.pressureForce, "N"},
        {"measured pressure", p.measuredPressure, "Pa"},
        {"flange clearance", p.flangeClearance, "m"},
        {"tread modulus", p.treadModulus, "Pa"},
        {"block area", p.blockArea, "m^2"},
        {"block radial stiffness (crown)", p.treadPositive * p.blockArea * p.treadModulus / (p.treadDepth + p.treadBase), "N/m"},
        {"max step", p.maxStep, "s"},
        {"max angle increment", p.maxAngleIncrement / kDeg, "deg"},
    };
}

ReportResult RunReport(const FlexRingData& data, const ReportOptions& options)
{
    ReportResult result;
    const std::string dir = options.outputDirectory;
    if (!dir.empty())
    {
        std::filesystem::create_directories(dir);
        WriteDataFile(data, std::filesystem::path(dir) / "tyre_data.tir");
    }
    std::mutex logMutex;
    const auto log = [&](const std::string& line) {
        if (options.log)
        {
            std::lock_guard<std::mutex> lock(logMutex);
            options.log(line);
        }
    };
    const auto cancelled = [&] {
        return options.cancel != nullptr && options.cancel->load();
    };

    // Pre-processing.
    PreprocessOptions pre;
    pre.fitStatic = options.fit;
    pre.fitModal = options.fit;
    pre.cancel = options.cancel;
    result.preprocess = Preprocess(data, pre);
    const FlexRingParameters& p = result.preprocess.parameters;
    log("Pre-processing: " + Fixed(result.preprocess.seconds, 2) + " s, " + std::to_string(result.preprocess.cycles) + " cycles, " +
        (result.preprocess.converged ? "converged" : "NOT converged"));
    for (const FitTarget& t : result.preprocess.targets)
    {
        log("  " + t.name + ": target " + Fixed(t.target, 4) + " " + t.unit + ", model " + Fixed(t.achieved, 4) + " (" + Fixed(100.0 * t.RelativeError(), 3) + " %)" +
            (t.fitted ? "" : " [reported]"));
    }
    for (const std::string& n : result.preprocess.notes)
    {
        log("  note: " + n);
    }
    {
        Csv csv(dir, "preprocess_targets.csv", {"name", "unit", "target", "achieved", "relative_error", "fitted"});
        for (const FitTarget& t : result.preprocess.targets)
        {
            csv.Row({t.target, t.achieved, t.RelativeError(), t.fitted ? 1.0 : 0.0}, t.name + "," + t.unit);
        }
        Csv params(dir, "parameters.csv", {"name", "unit", "value"});
        for (const ParameterRow& r : DescribeParameters(p))
        {
            params.Row({r.value}, r.name + "," + r.unit);
            if (options.preprocessOnly)
            {
                log("  " + r.name + " = " + General(r.value) + " " + r.unit);
            }
        }
    }
    if (options.preprocessOnly || cancelled())
    {
        return result;
    }

    // Modes.
    if (options.modal)
    {
        FlexRingTyre tyre(p);
        result.modal = AnalyzeUnloadedModes(tyre, 6);
        Csv csv(dir, "modal.csv", {"wave_number", "frequency_hz", "damping", "radial_share", "tangential_share", "lateral_share", "torsion_share"});
        for (const ModeInfo& m : result.modal.modes)
        {
            csv.Row({static_cast<double>(m.waveNumber), m.frequency, m.damping, m.share[0], m.share[1], m.share[2], m.share[3]});
        }
        log("Modes: f1 " + Fixed(result.modal.f1, 2) + " f2 " + Fixed(result.modal.f2, 2) + " f3 " + Fixed(result.modal.f3, 2) + " f4 " + Fixed(result.modal.f4, 2) + " f5 " +
            Fixed(result.modal.f5, 2) + " f6 " + Fixed(result.modal.f6, 2) + " Hz; subspace residual " + General(result.modal.subspaceResidual) + ", asymmetry " +
            General(result.modal.asymmetry));
    }

    // Statics: Fz(deflection), footprint, stiffness.
    const double d1 = data.firstDeflection / 1000.0;
    if (options.statics && !cancelled())
    {
        FlexRingTyre tyre(p);
        FlatRoad road(0.0);
        const std::vector<double> deflections = Range(0.0025, options.quick ? 0.03 : 0.04, options.quick ? 12 : 16);
        result.statics = StaticDeflection(tyre, road, RigPose{}, deflections, 0.0025);
        Csv csv(dir, "static_fz_deflection.csv", {"deflection_mm", "wheel_load_n", "rim_fz_n", "contact_blocks", "patch_length_mm", "patch_width_mm", "area_cm2", "mean_pressure_bar", "max_pressure_bar", "converged"});
        for (const StaticPoint& s : result.statics)
        {
            csv.Row({s.deflection * 1000.0, s.contact.normalForce, s.forces.fz, static_cast<double>(s.contact.blocks), s.contact.length * 1000.0, s.contact.width * 1000.0,
                     s.contact.area * 1.0e4, s.contact.meanPressure / 1.0e5, s.contact.maxPressure / 1.0e5, s.converged ? 1.0 : 0.0});
        }
        // The footprint at the first deflection.
        FlexRingTyre foot(p);
        StaticDeflection(foot, road, RigPose{}, {d1}, 0.0025);
        Csv fp(dir, "footprint.csv", {"x_mm", "y_mm", "pressure_bar", "normal_force_n", "fx_n", "fy_n", "sliding"});
        for (const BlockView& b : foot.Blocks())
        {
            if (b.contact)
            {
                result.footprint.push_back(b);
                fp.Row({b.tip.x * 1000.0, b.tip.y * 1000.0, b.groundPressure / 1.0e5, b.normalForce, b.force.x, b.force.y, b.sliding ? 1.0 : 0.0});
            }
        }
        log("Footprint at " + Fixed(d1 * 1000.0, 1) + " mm: " + Fixed(foot.Contact().normalForce, 1) + " N, " + std::to_string(foot.Contact().blocks) + " blocks, " +
            Fixed(foot.Contact().length * 1000.0, 1) + " x " + Fixed(foot.Contact().width * 1000.0, 1) + " mm, mean " + Fixed(foot.Contact().meanPressure / 1.0e5, 3) + " bar");
        // Cleats (FTire 6.4: Fz_decr_trans_cleat, Fz_decr_long_cleat).
        CleatGeometry transversal;
        transversal.width = 0.02;
        transversal.height = 0.05;
        CleatGeometry longitudinal = transversal;
        longitudinal.direction[0] = 0.0;
        longitudinal.direction[1] = 1.0;
        FlexRingTyre onCleat(p);
        CleatRoad tc(0.0, transversal);
        const double fzTrans = StaticDeflection(onCleat, tc, RigPose{}, {d1}, 0.0025).back().contact.normalForce;
        CleatRoad lc(0.0, longitudinal);
        const double fzLong = StaticDeflection(onCleat, lc, RigPose{}, {0.010}, 0.0025).back().contact.normalForce;
        const double fzFlat10 = StaticDeflection(foot, road, RigPose{}, {0.010}, 0.0025).back().contact.normalForce;
        RigPose cambered;
        cambered.camber = 6.0 * kDeg;
        FlexRingTyre camberTyre(p);
        const double fzCamber = StaticDeflection(camberTyre, road, cambered, {d1}, 0.0025).back().contact.normalForce;
        const double fzFlat = result.statics.empty() ? 0.0 : StaticDeflection(foot, road, RigPose{}, {d1}, 0.0025).back().contact.normalForce;
        result.stiffness = MeasureStaticStiffness(p, d1);
        Csv st(dir, "static_stiffness.csv", {"quantity", "unit", "value"});
        st.Row({result.stiffness.load}, "wheel_load_at_first_deflection,N");
        st.Row({result.stiffness.vertical}, "vertical_secant,N/m");
        st.Row({result.stiffness.longitudinal}, "tire_long_stiffn,N/m");
        st.Row({result.stiffness.lateral}, "tire_lat_stiffn,N/m");
        st.Row({result.stiffness.torsional}, "tire_tors_stiffn,N m/rad");
        st.Row({fzFlat > 0.0 ? 100.0 * (1.0 - fzTrans / fzFlat) : 0.0}, "Fz_decr_trans_cleat,%");
        st.Row({fzFlat10 > 0.0 ? 100.0 * (1.0 - fzLong / fzFlat10) : 0.0}, "Fz_decr_long_cleat (10 mm),%");
        st.Row({fzFlat > 0.0 ? 100.0 * (1.0 - fzCamber / fzFlat) : 0.0}, "Fz_decr_6deg_cam,%");
        log("Static stiffness: vertical " + Fixed(result.stiffness.vertical / 1000.0, 1) + " N/mm, longitudinal " + Fixed(result.stiffness.longitudinal / 1000.0, 1) +
            " N/mm, lateral " + Fixed(result.stiffness.lateral / 1000.0, 1) + " N/mm, torsional " + Fixed(result.stiffness.torsional * kDeg, 1) + " N m/deg");
        log("Fz decrease: transversal cleat " + Fixed(fzFlat > 0.0 ? 100.0 * (1.0 - fzTrans / fzFlat) : 0.0, 1) + " %, longitudinal cleat " +
            Fixed(fzFlat10 > 0.0 ? 100.0 * (1.0 - fzLong / fzFlat10) : 0.0, 1) + " %, 6 deg camber " + Fixed(fzFlat > 0.0 ? 100.0 * (1.0 - fzCamber / fzFlat) : 0.0, 1) + " %");
    }

    // Steady-state characteristics (Gipser 1999, figs. 7-12).
    if (options.sweeps && !cancelled())
    {
        const std::vector<double> loads = {2000.0, 4000.0, 6000.0, 8000.0};
        const int count = options.quick ? 8 : 16;
        SteadySettings base;
        base.speed = 60.0 / 3.6;
        if (options.quick)
        {
            base.settleTime = 0.2;
        }
        // Free rolling first: effective radius, rolling resistance.
        SteadySettings free = base;
        free.load = data.statWheelLoadAtFirstDefl;
        result.freeRolling = RunSteady(p, free);
        log("Free rolling 60 km/h at " + Fixed(free.load, 0) + " N: Re " + Fixed(result.freeRolling.effectiveRadius * 1000.0, 2) + " mm, Fx " + Fixed(result.freeRolling.forces.fx, 1) +
            " N, My " + Fixed(result.freeRolling.forces.my, 2) + " N m, " + Fixed(result.freeRolling.cpuSeconds, 2) + " s");

        std::vector<SweepSeries> series;
        for (const double load : loads)
        {
            SweepSeries a;
            a.kind = SweepKind::SlipAngle;
            a.load = load;
            a.name = "Fy_Mz_alpha Fz=" + Fixed(load, 0);
            series.push_back(a);
            SweepSeries k;
            k.kind = SweepKind::SlipRatio;
            k.load = load;
            k.name = "Fx_kappa Fz=" + Fixed(load, 0);
            series.push_back(k);
        }
        for (const double alpha : {0.0, 2.0, 4.0, 6.0, 8.0})
        {
            SweepSeries c;
            c.kind = SweepKind::SlipRatio;
            c.load = 4000.0;
            c.fixedSlipAngle = alpha * kDeg;
            c.name = "combined alpha=" + Fixed(alpha, 0);
            series.push_back(c);
        }
        for (const double gamma : {0.0, 2.0, 4.0, 6.0, 8.0})
        {
            SweepSeries c;
            c.kind = SweepKind::SlipAngle;
            c.load = 4000.0;
            c.fixedCamber = gamma * kDeg;
            c.name = "camber gamma=" + Fixed(gamma, 0);
            series.push_back(c);
        }
        const auto start = std::chrono::steady_clock::now();
        // MINIENGINE_FLEX_RING_SWEEPS=<text>: only the series whose names contain it.
        if (const char* only = std::getenv("MINIENGINE_FLEX_RING_SWEEPS"))
        {
            std::erase_if(series, [&](const SweepSeries& s) {
                return s.name.find(only) == std::string::npos;
            });
        }
        ParallelFor(static_cast<int>(series.size()), [&](int i) {
            SweepSeries& s = series[static_cast<size_t>(i)];
            SteadySettings b = base;
            b.load = s.load;
            b.slipAngle = s.fixedSlipAngle;
            b.camber = s.fixedCamber;
            // Dense where the force builds, sparse where it slides.
            std::vector<double> values;
            if (s.kind == SweepKind::SlipAngle)
            {
                const std::vector<double> full = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 10.0, 12.0, 15.0, 20.0};
                const std::vector<double> quick = {0.0, 1.0, 2.0, 3.0, 4.0, 6.0, 10.0, 20.0};
                for (double a : options.quick ? quick : full)
                {
                    values.push_back(a * kDeg);
                }
            }
            else
            {
                const std::vector<double> full = {0.0, 0.01, 0.02, 0.03, 0.04, 0.05, 0.07, 0.1, 0.13, 0.17, 0.22, 0.3, 0.45, 0.6, 0.8, 1.0};
                const std::vector<double> quick = {0.0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.7, 1.0};
                values = options.quick ? quick : full;
            }
            (void)count;
            s.points = RunSweep(p, s.kind, values, b, options.cancel);
            std::string cpu;
            double total = 0.0;
            for (const SteadyPoint& pt : s.points)
            {
                cpu += " " + Fixed(pt.cpuSeconds, 1);
                total += pt.cpuSeconds;
            }
            log("  sweep " + s.name + " done, " + Fixed(total, 1) + " s CPU:" + cpu);
        });
        log("Sweeps: " + Fixed(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 1) + " s wall");
        Csv csv(dir, "steady_state.csv", {"series", "load_target_n", "slip_angle_deg", "slip_ratio", "camber_deg", "fx_n", "fy_n", "fz_n", "mx_nm", "my_nm", "mz_nm", "effective_radius_mm", "sliding_blocks", "contact_blocks", "cpu_s"});
        for (const SweepSeries& s : series)
        {
            for (const SteadyPoint& pt : s.points)
            {
                csv.Row({s.load, pt.settings.slipAngle / kDeg, pt.settings.freeRolling ? 0.0 : pt.settings.slipRatio, pt.settings.camber / kDeg, pt.forces.fx, pt.forces.fy, pt.forces.fz,
                         pt.forces.mx, pt.forces.my, pt.forces.mz, pt.effectiveRadius * 1000.0, static_cast<double>(pt.contact.sliding), static_cast<double>(pt.contact.blocks), pt.cpuSeconds},
                        "\"" + s.name + "\"");
            }
        }
        result.sweeps = std::move(series);
    }

    // Cleat run-over (Gipser 1999, figs. 5 and 6), fixed spindle at 4 kN.
    if (options.cleat && !cancelled())
    {
        std::vector<CleatSeries> runs;
        CleatGeometry triangle;
        triangle.width = 0.2;
        triangle.height = 0.01;
        triangle.topWidth = 0.0;
        CleatGeometry bar;
        bar.width = 0.028;
        bar.height = 0.005;
        bar.bevel = 0.005;
        CleatGeometry square;
        square.width = 0.02;
        square.height = 0.02;
        const struct
        {
            const char* name;
            CleatGeometry g;
        } shapes[] = {{"triangle 200x10 mm", triangle}, {"bar 28x5 mm, 5 mm bevel", bar}, {"cleat 20x20 mm", square}};
        for (const auto& shape : shapes)
        {
            for (const double kmh : {40.0, 80.0, 120.0})
            {
                CleatSeries s;
                s.name = std::string(shape.name) + " " + Fixed(kmh, 0) + " km/h";
                s.speed = kmh / 3.6;
                s.cleat = shape.g;
                runs.push_back(s);
            }
        }
        ParallelFor(static_cast<int>(runs.size()), [&](int i) {
            CleatSeries& s = runs[static_cast<size_t>(i)];
            CleatSettings c;
            c.speed = s.speed;
            c.load = 4000.0;
            c.cleat = s.cleat;
            c.runUp = 0.5;
            c.runOut = std::max(0.4, s.speed * 0.1); // 0.1 s past the cleat, as the papers plot
            c.dt = 0.0002;
            s.samples = RunCleat(p, c, options.cancel);
            double fzMax = 0.0, fzMin = 1.0e30, fxMax = -1.0e30, fxMin = 1.0e30;
            for (const CleatSample& x : s.samples)
            {
                fzMax = std::max(fzMax, x.forces.fz);
                fzMin = std::min(fzMin, x.forces.fz);
                fxMax = std::max(fxMax, x.forces.fx);
                fxMin = std::min(fxMin, x.forces.fx);
            }
            log("  cleat " + s.name + ": Fz " + Fixed(fzMin, 0) + ".." + Fixed(fzMax, 0) + " N, Fx " + Fixed(fxMin, 0) + ".." + Fixed(fxMax, 0) + " N");
        });
        Csv csv(dir, "cleat.csv", {"run", "time_s", "position_m", "fx_n", "fy_n", "fz_n", "my_nm", "spin_rad_s"});
        for (const CleatSeries& s : runs)
        {
            for (const CleatSample& x : s.samples)
            {
                csv.Row({x.time, x.position, x.forces.fx, x.forces.fy, x.forces.fz, x.forces.my, x.spinRate}, "\"" + s.name + "\"");
            }
        }
        result.cleats = std::move(runs);
    }

    // Computing time: Gipser 1999 table 1's discretizations (herring-bone blocks at the given distance,
    // the given step, no angle limit), Gipser 2004 fig. 14's reference point, and this data's own settings.
    if (options.benchmark && !cancelled())
    {
        struct Case
        {
            std::string name;
            int segments;
            double blockDistance; // m; 0: the data's blocks
            double step;
            double angle; // deg
        };
        std::vector<Case> cases = {
            {"Gipser 1999 T1: 60 seg, 3 mm, 0.4 ms", 60, 0.003, 0.0004, 30.0},
            {"Gipser 1999 T1: 60 seg, 0.75 mm, 0.4 ms", 60, 0.00075, 0.0004, 30.0},
            {"Gipser 1999 T1: 120 seg, 0.75 mm, 0.4 ms", 120, 0.00075, 0.0004, 30.0},
            {"Gipser 1999 T1: 60 seg, 3 mm, 0.2 ms", 60, 0.003, 0.0002, 30.0},
            {"Gipser 1999 T1: 60 seg, 0.75 mm, 0.2 ms", 60, 0.00075, 0.0002, 30.0},
            {"Gipser 1999 T1: 120 seg, 0.75 mm, 0.2 ms", 120, 0.00075, 0.0002, 30.0},
            {"Gipser 2004 fig. 14: 80 seg, 1 mm, 0.5 ms", 80, 0.001, 0.0005, 30.0},
            {"data: " + std::to_string(p.segments) + " seg, " + std::to_string(p.blocks.size()) + " blocks, " + Fixed(p.maxStep * 1000.0, 2) + " ms, " +
                 Fixed(p.maxAngleIncrement / kDeg, 1) + " deg",
             0, 0.0, 0.0, 0.0},
        };
        Csv csv(dir, "benchmark.csv", {"case", "segments", "blocks_per_segment", "max_step_ms", "steps_per_s", "us_per_step", "rtf_one_tyre", "rtf_four_tyres_parallel", "blocks_in_contact"});
        // MINIENGINE_FLEX_RING_BENCH=<text>: only the cases whose names contain it.
        if (const char* only = std::getenv("MINIENGINE_FLEX_RING_BENCH"))
        {
            std::erase_if(cases, [&](const Case& c) {
                return c.name.find(only) == std::string::npos;
            });
        }
        for (const Case& c : cases)
        {
            if (cancelled())
            {
                break;
            }
            FlexRingData d = data;
            if (c.segments > 0)
            {
                d.numberBeltSegments = c.segments;
                const double circumference = 2.0 * std::numbers::pi * p.beltRadius;
                d.numberBlocksPerBeltSegm = std::max(1.0, std::round(circumference / c.segments / c.blockDistance));
                d.treadDiscretizationType = 0;
                d.maximumTimeStep = c.step;
                d.maximumAngleIncrement = c.angle;
            }
            // The structure fitted once at the data's discretization; the benchmark only needs a sound one.
            // Each discretization pre-processed (fitted) on its own: the same tyre, its own segments.
            const FlexRingParameters bp = c.segments > 0 ? Preprocess(d, pre).parameters : p;
            BenchmarkCase bc;
            bc.name = c.name;
            bc.result = RunBenchmark(bp, options.quick ? 0.25 : 0.5, true);
            result.benchmarks.push_back(bc);
            csv.Row({static_cast<double>(bc.result.segments), static_cast<double>(bc.result.blocksPerSegment), bc.result.maxStep * 1000.0, bc.result.stepsPerSecond,
                     bc.result.microsecondsPerStep, bc.result.secondsPerSimulatedSecond, bc.result.parallelFourTyres, static_cast<double>(bc.result.blocksInContact)},
                    "\"" + c.name + "\"");
            log("  " + c.name + ": " + Fixed(bc.result.microsecondsPerStep, 1) + " us/step, RTF " + Fixed(bc.result.secondsPerSimulatedSecond, 3) + ", four tyres " +
                Fixed(bc.result.parallelFourTyres, 3) + ", " + std::to_string(bc.result.blocksInContact) + " blocks in contact");
        }
    }

    // A short JSON summary for scripts.
    if (!dir.empty())
    {
        std::ofstream json(std::filesystem::path(dir) / "summary.json");
        json << "{\n  \"preprocess\": {\"seconds\": " << General(result.preprocess.seconds) << ", \"converged\": " << (result.preprocess.converged ? "true" : "false") << ", \"targets\": [";
        for (size_t i = 0; i < result.preprocess.targets.size(); ++i)
        {
            const FitTarget& t = result.preprocess.targets[i];
            json << (i ? ", " : "") << "{\"name\": \"" << t.name << "\", \"target\": " << General(t.target) << ", \"achieved\": " << General(t.achieved) << "}";
        }
        json << "]},\n  \"modal\": {\"f1\": " << General(result.modal.f1) << ", \"f2\": " << General(result.modal.f2) << ", \"f3\": " << General(result.modal.f3) << ", \"f4\": " << General(result.modal.f4)
             << ", \"f5\": " << General(result.modal.f5) << ", \"f6\": " << General(result.modal.f6) << "},\n";
        json << "  \"stiffness\": {\"vertical\": " << General(result.stiffness.vertical) << ", \"longitudinal\": " << General(result.stiffness.longitudinal) << ", \"lateral\": " << General(result.stiffness.lateral)
             << ", \"torsional\": " << General(result.stiffness.torsional) << "},\n";
        json << "  \"free_rolling\": {\"effective_radius\": " << General(result.freeRolling.effectiveRadius) << ", \"fx\": " << General(result.freeRolling.forces.fx) << "},\n";
        json << "  \"benchmark\": [";
        for (size_t i = 0; i < result.benchmarks.size(); ++i)
        {
            const BenchmarkCase& b = result.benchmarks[i];
            json << (i ? ", " : "") << "{\"case\": \"" << b.name << "\", \"us_per_step\": " << General(b.result.microsecondsPerStep) << ", \"rtf\": " << General(b.result.secondsPerSimulatedSecond)
                 << ", \"rtf_four\": " << General(b.result.parallelFourTyres) << "}";
        }
        json << "]\n}\n";
    }
    return result;
}

}
