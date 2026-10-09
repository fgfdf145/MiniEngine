// The flexible ring tyre (engine/tyre/flex_ring): its numerics against closed forms and dense algebra, its
// statics, fit and rolling against physics' signs and bounds, and its C interface.

#include <engine/tyre/flex_ring/flex_ring_banded.h>
#include <engine/tyre/flex_ring/flex_ring_c_api.h>
#include <engine/tyre/flex_ring/flex_ring_data.h>
#include <engine/tyre/flex_ring/flex_ring_modal.h>
#include <engine/tyre/flex_ring/flex_ring_preprocess.h>
#include <engine/tyre/flex_ring/flex_ring_rig.h>
#include <engine/tyre/flex_ring/flex_ring_tyre.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me::tyre::flexring;

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireNear(double actual, double expected, double tolerance, const std::string& what)
{
    if (!(std::abs(actual - expected) <= tolerance))
    {
        throw std::runtime_error(what + ": got " + std::to_string(actual) + ", expected " + std::to_string(expected) + " (tolerance " + std::to_string(tolerance) + ")");
    }
}

// The default data's model, fitted once for all tests.
const PreprocessResult& Fitted()
{
    static const PreprocessResult result = Preprocess(MakeDefaultData());
    return result;
}

void TestBandSolverMatchesDense()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const int n = 40;
    const int b = 6;
    std::vector<double> dense(static_cast<size_t>(n) * n, 0.0);
    BandMatrix band;
    band.Resize(n, b);
    for (int i = 0; i < n; ++i)
    {
        for (int j = std::max(0, i - b); j <= i; ++j)
        {
            const double v = i == j ? 20.0 + u(rng) : u(rng);
            dense[static_cast<size_t>(i) * n + j] = v;
            dense[static_cast<size_t>(j) * n + i] = v;
            band.Add(i, j, v);
        }
    }
    std::vector<double> rhs(n);
    for (double& v : rhs)
    {
        v = u(rng);
    }
    std::vector<double> x = rhs;
    Require(band.Factor(), "band matrix factors");
    band.Solve(x.data());
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;
        for (int j = 0; j < n; ++j)
        {
            sum += dense[static_cast<size_t>(i) * n + j] * x[j];
        }
        RequireNear(sum, rhs[i], 1.0e-12, "band solve residual");
    }
}

void TestFreeBeamShapes()
{
    const std::vector<double> roots = FreeBeamRoots(5);
    const double expected[5] = {4.7300407449, 7.8532046241, 10.9956078380, 14.1371654913, 17.2787596574};
    for (int i = 0; i < 5; ++i)
    {
        RequireNear(roots[i], expected[i], 1.0e-8, "free-free beam root " + std::to_string(i + 1));
    }
    // Orthonormal, and orthogonal to the rigid motions (1 and s) the node translation and torsion carry.
    const FlexRingTyre tyre(BuildParameters(MakeDefaultData()));
    const double w = tyre.Parameters().beltWidth;
    const int modes = static_cast<int>(tyre.Parameters().bendShapeRoots.size());
    Require(modes == 4, "four shapes by default");
    constexpr int kSamples = 4000;
    for (int i = 0; i < modes; ++i)
    {
        for (int j = 0; j < modes; ++j)
        {
            double sum = 0.0, rigid0 = 0.0, rigid1 = 0.0;
            for (int s = 0; s <= kSamples; ++s)
            {
                const double y = -0.5 * w + w * s / kSamples;
                const double weight = (s == 0 || s == kSamples ? 0.5 : 1.0) * w / kSamples;
                sum += weight * tyre.BendShape(i, y) * tyre.BendShape(j, y);
                rigid0 += weight * tyre.BendShape(i, y);
                rigid1 += weight * tyre.BendShape(i, y) * y;
            }
            RequireNear(sum, i == j ? 1.0 : 0.0, 1.0e-5, "shape inner product " + std::to_string(i) + "," + std::to_string(j));
            RequireNear(rigid0, 0.0, 1.0e-6, "shape orthogonal to translation");
            RequireNear(rigid1, 0.0, 1.0e-6, "shape orthogonal to rotation");
        }
    }
}

void TestRestIsEquilibrium()
{
    FlexRingTyre tyre(BuildParameters(MakeDefaultData()));
    tyre.Reset(RimState{});
    std::vector<Vec3> force;
    std::vector<double> torque;
    const std::vector<Vec3> v(tyre.Segments(), Vec3(0.0));
    const std::vector<double> z(tyre.Segments(), 0.0);
    tyre.StructuralForces(tyre.NodePositions(), v, tyre.Torsion(), z, force, torque);
    double worst = 0.0;
    for (int k = 0; k < tyre.Segments(); ++k)
    {
        worst = std::max(worst, glm::length(force[k]));
        worst = std::max(worst, std::abs(torque[k]));
    }
    // Pressure on each node (~540 N) balanced by the chords' pre-tension: no residual force.
    Require(worst < 1.0e-6 * tyre.Parameters().pressureForce, "the inflated ring at rest is in equilibrium (residual " + std::to_string(worst) + " N)");
}

void TestModesMatchRigidRingClosedForms()
{
    // Without pressure, bending and twist the ring's rigid modes are closed forms: w1^2 = c_t / m,
    // w2^2 = (c_r + c_t) / 2m, w3^2 = c_y / m; and with nothing to couple the torsion to the lateral motion
    // the w = 1 out-of-plane mode is pure lateral (w^2 = c_y / m) and the torsion one pure (w^2 = c_psi / J).
    FlexRingParameters p = BuildParameters(MakeDefaultData());
    p.pressureForce = 0.0;
    p.chordRestLength = p.chord;
    p.bendInStiffness = 0.0;
    p.bendOutStiffness = 0.0;
    p.twistStiffness = 0.0;
    p.torsionLateralCoupling = 0.0;
    FlexRingTyre tyre(p);
    const ModalResult m = AnalyzeUnloadedModes(tyre, 2);
    const double r = p.beltRadius;
    const auto hz = [](double w2) {
        return std::sqrt(w2) / (2.0 * kPi);
    };
    RequireNear(m.f1, hz(p.tangentialStiffness / p.nodeMass), 1.0e-6 * m.f1, "f1 closed form");
    // The rigid translation is the inextensible belt's w = 1 mode; the belt's finite extension stiffness
    // lets the true mode stretch a little below it.
    const double f2Rigid = hz((p.radialStiffness + p.tangentialStiffness) / (2.0 * p.nodeMass));
    Require(m.f2 <= f2Rigid * (1.0 + 1.0e-9) && m.f2 > 0.995 * f2Rigid, "f2 just below the rigid translation's (" + std::to_string(m.f2) + " vs " + std::to_string(f2Rigid) + ")");
    RequireNear(m.f3, hz(p.lateralStiffness / p.nodeMass), 1.0e-6 * m.f3, "f3 closed form");
    RequireNear(m.f4, hz(p.lateralStiffness / p.nodeMass), 1.0e-6 * m.f4, "f4: pure lateral when uncoupled");
    double torsionMode = 0.0;
    for (const ModeInfo& mode : m.modes)
    {
        if (mode.waveNumber == 1 && mode.share[3] > 0.5)
        {
            torsionMode = mode.frequency;
        }
    }
    RequireNear(torsionMode, hz(p.torsionStiffness / p.nodeInertia), 1.0e-6 * torsionMode, "torsion mode closed form");
    (void)r;
    Require(m.subspaceResidual < 1.0e-6, "the unloaded ring is block-circulant");
}

// The continuous ring on an elastic foundation (extension K, bending D on the second derivative's normal
// component, foundation k_r, k_t per length, mass rho A per length), mode cos(n theta): its lowest
// in-plane frequency.
double ContinuousRingHz(int n, double a, double K, double D, double kr, double kt, double rhoA)
{
    const double c = n * n + 1.0;
    const double k11 = K / (a * a) + D * c * c / std::pow(a, 4) + kr;
    const double k12 = K * n / (a * a) + D * 2.0 * n * c / std::pow(a, 4);
    const double k22 = K * n * n / (a * a) + D * 4.0 * n * n / std::pow(a, 4) + kt;
    const double tr = k11 + k22;
    const double det = k11 * k22 - k12 * k12;
    const double lowest = 0.5 * (tr - std::sqrt(tr * tr - 4.0 * det));
    return std::sqrt(lowest / rhoA) / (2.0 * kPi);
}

void TestDiscreteRingConvergesToTheContinuousRing()
{
    double lastError = 1.0e300;
    for (const int segments : {50, 100, 200})
    {
        FlexRingData d = MakeDefaultData();
        d.numberBeltSegments = segments;
        FlexRingParameters p = BuildParameters(d);
        p.pressureForce = 0.0;
        p.chordRestLength = p.chord;
        // Per-length values held as the segment count changes.
        const double perLengthRadial = 2.0e6;
        const double perLengthTangential = 6.0e5;
        const double rhoA = 3.5;
        const double ei = 2.0;
        const double ea = 8.0e5;
        p.radialStiffness = perLengthRadial * p.chord;
        p.tangentialStiffness = perLengthTangential * p.chord;
        p.nodeMass = rhoA * p.chord;
        p.bendInStiffness = ei / std::pow(p.chord, 3);
        p.chordStiffness = ea / p.chord;
        p.chordDamping = 0.0;
        FlexRingTyre tyre(p);
        const ModalResult m = AnalyzeUnloadedModes(tyre, 2);
        const double exact = ContinuousRingHz(2, p.beltRadius, ea, ei, perLengthRadial, perLengthTangential, rhoA);
        const double error = std::abs(m.f5 / exact - 1.0);
        std::cout << "    " << segments << " segments: f(n=2) " << m.f5 << " Hz, continuous ring " << exact << " Hz, error " << error * 100.0 << " %\n";
        Require(error < lastError, "the error falls as the ring is refined");
        lastError = error;
    }
    Require(lastError < 0.005, "200 segments within 0.5 % of the continuous ring");
}

void TestStaticLoad()
{
    FlexRingTyre tyre(Fitted().parameters);
    FlatRoad road(0.0);
    const std::vector<StaticPoint> points = StaticDeflection(tyre, road, RigPose{}, {0.005, 0.010, 0.015, 0.020});
    double last = 0.0;
    for (const StaticPoint& s : points)
    {
        Require(s.converged, "static point settles");
        Require(s.contact.normalForce > last, "wheel load grows with deflection");
        Require(std::abs(s.forces.fy) < 0.01 * s.contact.normalForce, "no side force standing straight");
        Require(std::abs(s.forces.fx) < 0.01 * s.contact.normalForce, "no fore-aft force standing straight");
        Require(std::abs(s.contact.centre.x) < 0.005 && std::abs(s.contact.centre.y) < 0.005, "the patch centred under the wheel");
        last = s.contact.normalForce;
    }
    // The fit's first point (10 mm, the game's rate).
    RequireNear(points[1].contact.normalForce, MakeDefaultData().statWheelLoadAtFirstDefl, 0.002 * MakeDefaultData().statWheelLoadAtFirstDefl, "wheel load at 10 mm");
}

void TestFitHitsItsTargets()
{
    const PreprocessResult& r = Fitted();
    for (const FitTarget& t : r.targets)
    {
        std::cout << "    " << t.name << ": target " << t.target << ", model " << t.achieved << "\n";
    }
    for (const FitTarget& t : r.targets)
    {
        if (t.name == "stat_wheel_load_at_first_defl" || t.name == "f1" || t.name == "f4" || t.name == "f6")
        {
            RequireNear(t.achieved, t.target, 1.0e-3 * t.target, t.name);
        }
    }
}

void TestFreeRolling()
{
    SteadySettings s;
    s.load = 3000.0;
    s.settleTime = 0.3;
    const SteadyPoint p = RunSteady(Fitted().parameters, s);
    const double outer = Fitted().parameters.outerRadius;
    std::cout << "    Re " << p.effectiveRadius << " m (loaded " << outer - p.deflection << ", unloaded " << outer << "), Fx " << p.forces.fx << " N, Fz " << p.forces.fz << " N\n";
    Require(p.effectiveRadius > outer - p.deflection && p.effectiveRadius < outer, "effective radius between loaded and unloaded radius");
    Require(p.forces.fx < 0.0 && p.forces.fx > -0.03 * s.load, "a small rolling resistance");
    RequireNear(p.forces.fz, s.load, 0.03 * s.load, "load held");
}

void TestSlipSigns()
{
    SteadySettings s;
    s.load = 3000.0;
    s.settleTime = 0.25;
    s.slipAngle = 2.0 * kDeg;
    const SteadyPoint side = RunSteady(Fitted().parameters, s);
    std::cout << "    alpha 2 deg: Fy " << side.forces.fy << " N, Mz " << side.forces.mz << " N m\n";
    Require(side.forces.fy > 0.3 * s.load, "ISO: positive slip angle, positive side force");
    Require(side.forces.mz < 0.0, "aligning torque turns the wheel back");
    SteadySettings drive = s;
    drive.slipAngle = 0.0;
    drive.freeRolling = false;
    drive.slipRatio = 0.05;
    const SteadyPoint a = RunSteady(Fitted().parameters, drive);
    drive.slipRatio = -0.05;
    const SteadyPoint b = RunSteady(Fitted().parameters, drive);
    std::cout << "    kappa +-5 %: Fx " << a.forces.fx << " / " << b.forces.fx << " N\n";
    Require(a.forces.fx > 0.3 * s.load, "driving slip pushes forward");
    Require(b.forces.fx < -0.3 * s.load, "braking slip pulls back");
}

void TestCleatRaisesTheLoad()
{
    CleatSettings c;
    c.speed = 40.0 / 3.6;
    c.load = 3000.0;
    c.cleat.width = 0.02;
    c.cleat.height = 0.01;
    c.runUp = 0.3;
    c.runOut = 0.3;
    const std::vector<CleatSample> samples = RunCleat(Fitted().parameters, c);
    double peak = 0.0;
    double minFx = 0.0;
    for (const CleatSample& s : samples)
    {
        Require(std::isfinite(s.forces.fz), "finite forces over the cleat");
        peak = std::max(peak, s.forces.fz);
        minFx = std::min(minFx, s.forces.fx);
    }
    std::cout << "    peak Fz " << peak << " N, most negative Fx " << minFx << " N\n";
    Require(peak > 1.3 * c.load, "the cleat raises the wheel load");
    Require(minFx < -0.1 * c.load, "the cleat pushes the wheel back");
}

void TestDataFileRoundTrip()
{
    FlexRingData d = MakeDefaultData();
    d.numberTreadStrips = 13;
    d.muSlidingAtHighP = 0.777;
    std::ostringstream out;
    WriteData(d, out);
    std::istringstream in(out.str());
    const DataReadResult r = ReadData(in);
    Require(r.unknownKeys.empty(), "every written key is known");
    for (const FieldInfo& f : Fields())
    {
        if (f.kind == FieldKind::Real)
        {
            RequireNear(r.data.*(f.real), d.*(f.real), 1.0e-9 * std::max(1.0, std::abs(d.*(f.real))), std::string("round trip of ") + f.name);
        }
    }
    Require(r.data.name == d.name, "name survives");
}

void TestFrictionTable()
{
    const FlexRingParameters p = BuildParameters(MakeDefaultData());
    const FrictionTable& f = p.friction;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            RequireNear(f.Mu(f.speed[j], f.pressure[i]), f.mu[i][j], 1.0e-12, "friction table node");
        }
    }
    RequireNear(f.Mu(1000.0, f.pressure[1]), f.mu[1][3], 1.0e-12, "constant past the blocking speed");
}

void TestCApi()
{
    Require(mefr_version() == MEFR_VERSION, "version");
    mefr_tyre* t = mefr_create(nullptr);
    Require(t != nullptr, "create");
    Require(mefr_set_data(t, "number_belt_segments", 60) == 0, "set data");
    double v = 0.0;
    Require(mefr_get_data(t, "NUMBER_BELT_SEGMENTS", &v) == 0 && v == 60.0, "get data, any case");
    Require(mefr_set_data(t, "no_such_item", 1.0) != 0, "unknown item refused");
    Require(mefr_preprocess(t, 0) == 0, "preprocess");
    Require(mefr_node_count(t) == 60, "node count");
    mefr_road_flat(t, 0.0, 1.0);
    const double rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double r0 = 0.3266 - 0.01;
    const double pos[3] = {0.0, 0.0, r0};
    const double zero[3] = {0.0, 0.0, 0.0};
    Require(mefr_reset(t, pos, rot, zero, zero) == 0, "reset");
    Require(mefr_settle(t, pos, rot) == 0, "settle");
    double force[3] = {}, moment[3] = {};
    Require(mefr_advance(t, pos, rot, zero, zero, 0.001, force, moment) == 0, "advance");
    Require(force[2] > 500.0, "the loaded tyre pushes the rim up");
    mefr_contact c{};
    mefr_contact_stats(t, &c);
    Require(c.blocks > 0 && c.normal_force > 500.0, "contact stats");
    std::vector<double> nodes(3 * 60);
    mefr_nodes(t, nodes.data(), nullptr);
    Require(std::abs(nodes[2] - (r0 - 0.3186)) < 0.05, "nodes read back");
    mefr_destroy(t);
}
}

int main()
{
    const struct
    {
        const char* name;
        void (*run)();
    } tests[] = {
        {"TestBandSolverMatchesDense", TestBandSolverMatchesDense},
        {"TestFreeBeamShapes", TestFreeBeamShapes},
        {"TestRestIsEquilibrium", TestRestIsEquilibrium},
        {"TestModesMatchRigidRingClosedForms", TestModesMatchRigidRingClosedForms},
        {"TestDiscreteRingConvergesToTheContinuousRing", TestDiscreteRingConvergesToTheContinuousRing},
        {"TestFrictionTable", TestFrictionTable},
        {"TestDataFileRoundTrip", TestDataFileRoundTrip},
        {"TestFitHitsItsTargets", TestFitHitsItsTargets},
        {"TestStaticLoad", TestStaticLoad},
        {"TestFreeRolling", TestFreeRolling},
        {"TestSlipSigns", TestSlipSigns},
        {"TestCleatRaisesTheLoad", TestCleatRaisesTheLoad},
        {"TestCApi", TestCApi},
    };
    int failed = 0;
    for (const auto& test : tests)
    {
        try
        {
            test.run();
            std::cout << "[PASS] " << test.name << "\n";
        }
        catch (const std::exception& e)
        {
            std::cout << "[FAIL] " << test.name << ": " << e.what() << "\n";
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}
