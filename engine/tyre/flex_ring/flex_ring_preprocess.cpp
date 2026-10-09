#include "flex_ring_preprocess.h"

#include "flex_ring_rig.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <sstream>

namespace me::tyre::flexring
{

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kDegree = kPi / 180.0;

double Omega(double hz)
{
    return 2.0 * kPi * hz;
}

// A deterministic jitter in [-0.5, 0.5) for the pseudo-random block placement.
double Jitter(int a, int b)
{
    const double v = std::sin(a * 12.9898 + b * 78.233) * 43758.5453;
    return v - std::floor(v) - 0.5;
}

// Everything that follows from the fitted primary values (mass, radial, tangential, lateral and torsion
// stiffness): dampers from the modal damping, Maxwell and hysteresis elements, the belt's extension
// spring and its pre-tension under pressure, the rim flange.
void Derive(const FlexRingData& d, FlexRingParameters& p)
{
    const int n = p.segments;
    const double m = p.nodeMass;
    const double r = p.beltRadius;
    p.nodeInertia = m * p.beltWidth * p.beltWidth / 12.0;
    const double j = p.nodeInertia;
    const double w1 = Omega(d.f1);
    const double w2 = Omega(d.f2);
    const double w4 = Omega(d.f4);
    p.tangentialDamping = 2.0 * d.d1 * m * w1;
    p.radialDamping = std::max(4.0 * d.d2 * m * w2 - p.tangentialDamping, 0.0);
    const double torsionShare = d.beltTorsionTwistDamp / 100.0;
    p.torsionDamping = 2.0 * torsionShare * std::sqrt(std::max(p.torsionStiffness, 0.0) * j);
    p.twistDamping = 2.0 * torsionShare * std::sqrt(std::max(p.twistStiffness, 0.0) * 0.5 * j);
    p.lateralDamping = std::max((2.0 * d.d4 * w4 * (m * r * r + j) - p.torsionDamping) / (r * r), 0.0);

    p.maxwellStiffness = {d.radDynamicStiffening / 100.0 * p.radialStiffness, d.tangDynamicStiffening / 100.0 * p.tangentialStiffness,
                          d.latDynamicStiffening / 100.0 * p.lateralStiffness};
    p.maxwellTime = std::max(d.timeConstDynamicStiffening, 1.0e-6);

    // Hysteresis: the loop widths are the whole tyre's with 30 deg of tread in contact (n / 12 nodes); five
    // radial sliders of equal stiffness and thresholds 1..5 / 15 of the node's share (an Iwan model).
    const double nodesIn30 = std::max(n / 12.0, 1.0);
    const double radialShare = 0.5 * d.radialHysteresisForce / nodesIn30;
    for (int e = 0; e < 5; ++e)
    {
        p.radialHysteresisStiffness[e] = d.radialHystereticStiffening / 100.0 * p.radialStiffness / 5.0;
        p.radialHysteresisForce[e] = radialShare * (e + 1) / 15.0;
    }
    p.tangentialHysteresisStiffness = d.tangHystereticStiffening / 100.0 * p.tangentialStiffness;
    p.tangentialHysteresisForce = 0.5 * d.tangHysteresisForce / nodesIn30;
    p.lateralHysteresisStiffness = d.latHystereticStiffening / 100.0 * p.lateralStiffness;
    p.lateralHysteresisForce = 0.5 * d.latHysteresisForce / nodesIn30;

    // Extension: the centrifugal growth at the maximum speed (FTire 6.4, belt_extension_due_to_vmax),
    // m w^2 (r + dr) = (c_r + 4 c_long sin^2(pi/n)) dr.
    const double growth = std::max(d.beltExtensionDueToVmax, 0.01) / 100.0 * r;
    const double omegaMax = p.maxSpeed / p.outerRadius;
    const double sinHalf = std::sin(kPi / n);
    double chordStiffness = (m * omegaMax * omegaMax * (r + growth) / growth - p.radialStiffness) / (4.0 * sinHalf * sinHalf);
    chordStiffness = std::max(chordStiffness, 20.0 * p.radialStiffness);
    p.chordStiffness = chordStiffness;
    p.chordDamping = 2.0 * d.beltExtensionDamp / 100.0 * std::sqrt(chordStiffness * 0.5 * m);
    // Pressure: 0.5 of p * area on each node (FTire 6.3), balanced at rest by the chords' pre-tension.
    p.measuredPressure = std::max(d.inflationPressure, 0.01) * 1.0e5;
    p.pressureForce = d.pressureForceFraction * (2.0 * kPi * r / n) * p.treadWidth * p.measuredPressure;
    const double tension = p.pressureForce / (2.0 * sinHalf);
    p.chordRestLength = p.chord - tension / chordStiffness;

    p.flangeClearance = d.rimFlangeClearance > 0.0 ? d.rimFlangeClearance / 1000.0 : 0.7 * p.sectionHeight;
    p.flangeStiffness = d.rimFlangeStiffnessFactor * p.radialStiffness;
}

struct Secant
{
    double x0 = 0.0, y0 = 0.0;
    bool has = false;
    // Next log(x) for log(y) to reach log(target), slope guess `s` until two points exist.
    double Next(double x, double y, double target, double s, double minSlope = 0.02, double maxLogStep = 0.693)
    {
        const double lx = std::log(x);
        const double ly = std::log(std::max(y, 1.0e-300));
        double slope = s;
        if (has && std::abs(lx - x0) > 1.0e-12)
        {
            slope = (ly - y0) / (lx - x0);
            if (!(slope > minSlope))
            {
                slope = s;
            }
        }
        x0 = lx;
        y0 = ly;
        has = true;
        double step = (std::log(target) - ly) / slope;
        step = std::clamp(step, -maxLogStep, maxLogStep);
        return std::exp(lx + step);
    }
};
}

std::vector<double> FreeBeamRoots(int count)
{
    std::vector<double> roots;
    for (int i = 1; i <= count; ++i)
    {
        double x = (i + 0.5) * kPi;
        for (int it = 0; it < 50; ++it)
        {
            // f = cos x - 1 / cosh x
            const double f = std::cos(x) - 1.0 / std::cosh(x);
            const double df = -std::sin(x) + std::tanh(x) / std::cosh(x);
            const double dx = f / df;
            x -= dx;
            if (std::abs(dx) < 1.0e-14)
            {
                break;
            }
        }
        roots.push_back(x);
    }
    return roots;
}

FlexRingParameters BuildParameters(const FlexRingData& d)
{
    FlexRingParameters p;
    const int n = std::clamp(static_cast<int>(std::lround(d.numberBeltSegments)), 12, 400);
    p.segments = n;
    const double rimRadius = d.rimDiameter * 0.0254 * 0.5;
    const double outer = d.unloadedRadius > 0.0 ? d.unloadedRadius / 1000.0 : rimRadius + d.tireSectionWidth / 1000.0 * d.tireAspectRatio / 100.0;
    p.treadDepth = d.treadDepth / 1000.0;
    p.treadBase = d.treadBaseHeight / 1000.0;
    const double rubber = p.treadDepth + p.treadBase;
    p.beltRadius = d.rollingCircumference > 0.0 ? d.rollingCircumference / 1000.0 / (2.0 * kPi) : outer - rubber;
    p.outerRadius = p.beltRadius + rubber;
    p.rimRadius = rimRadius;
    p.sectionHeight = std::max(p.outerRadius - p.rimRadius, 0.01);
    p.treadWidth = d.treadWidth > 0.0 ? d.treadWidth / 1000.0 : d.tireSectionWidth / 1000.0 - 2.0 * 0.15 * p.sectionHeight;
    p.beltWidth = d.beltWidth > 0.0 ? d.beltWidth / 1000.0 : 0.95 * p.treadWidth;
    p.latCurvatureRadius = std::max(d.beltLatCurvatureRadius / 1000.0, 0.6 * p.treadWidth);
    p.chord = 2.0 * p.beltRadius * std::sin(kPi / n);
    p.maxSpeed = MaxSpeedKmh(d.speedSymbol) / 3.6;

    const double freeMass = d.freeMassPercentage > 0.0 ? d.freeMassPercentage / 100.0 * d.tireMass : 0.7 * d.tireMass;
    p.nodeMass = freeMass / n;
    p.rimFixedMass = d.tireMass - freeMass;
    p.nodeInertia = p.nodeMass * p.beltWidth * p.beltWidth / 12.0;

    // First guesses from the rigid-ring closed forms: w1^2 = c_t / m, w2^2 = (c_r + c_t) / 2m,
    // w3^2 = c_y / m, w4^2 = (c_y r^2 + c_psi) / (m r^2 + J).
    const double m = p.nodeMass;
    const double r = p.beltRadius;
    const double w1 = Omega(d.f1);
    const double w2 = Omega(d.f2);
    const double w4 = Omega(d.f4);
    p.tangentialStiffness = m * w1 * w1;
    const double kv = d.firstDeflection > 0.0 ? d.statWheelLoadAtFirstDefl / (d.firstDeflection / 1000.0) : 2.5e5;
    p.radialStiffness = std::max(m * (2.0 * w2 * w2 - w1 * w1), 0.05 * kv);
    p.radialProgressivity = 0.0;
    p.radialProgressionScale = std::max(d.firstDeflection / 1000.0, 0.002);
    if (d.f3 > 0.0)
    {
        const double w3 = Omega(d.f3);
        p.lateralStiffness = m * w3 * w3;
        p.torsionStiffness = std::max((m * r * r + p.nodeInertia) * w4 * w4 - p.lateralStiffness * r * r, 1.0e-3 * p.lateralStiffness * r * r);
    }
    else
    {
        p.torsionStiffness = d.beltTorsionStiffn / kDegree * p.chord;
        p.lateralStiffness = std::max(((m * r * r + p.nodeInertia) * w4 * w4 - p.torsionStiffness) / (r * r), 1.0);
    }
    p.twistStiffness = d.beltTwistStiffn / kDegree / p.chord;
    p.torsionLateralCoupling = d.beltTorsionLatDisplCoupl * kDegree * 1000.0;
    p.torsionBendCoupling = d.beltTorsionOopBendCoupl;

    const double chord3 = p.chord * p.chord * p.chord;
    const double eiIn = d.f5 > 0.0 ? 2.0 : d.beltInPlaneBendStiffn;
    const double eiOut = d.f6 > 0.0 ? 50.0 : d.beltOutOfPlaneBendStiffn;
    p.bendInStiffness = eiIn / chord3;
    p.bendOutStiffness = eiOut / chord3;

    const int modes = std::clamp(static_cast<int>(std::lround(d.numberBeltBendShapeFunct)), 0, 12);
    p.bendShapeRoots = FreeBeamRoots(modes);
    p.lateralBendStiffness = d.beltLatBendStiffn * p.chord;
    p.lateralBendDamping = d.beltLatBendDamp;
    p.lateralBendCoupling = d.beltLatBendStiffnLongCoupl;

    // Tread (FTire 6.2, 6.5).
    p.treadModulus = d.youngsModTreadRubber > 0.0 ? d.youngsModTreadRubber * 1.0e6 : std::pow(10.0, 5.33905 + 0.020477 * d.stiffnessTreadRubber);
    p.treadPositive = std::clamp(d.treadPositive / 100.0, 0.01, 1.0);
    p.shearFactor = d.treadPatternShapeFactorTang / 3.0;
    p.lateralShearRatio = d.latToLongTreadStiffnRatio;
    p.treadProgressivity = d.stiffnProgrTreadRubber / 100.0;
    p.treadDampingTime = d.dampingTreadRubber;

    const int strips = std::max(static_cast<int>(std::lround(d.numberTreadStrips)), 1);
    int blocks = std::max(static_cast<int>(std::lround(d.numberBlocksPerBeltSegm)), 1);
    const int type = static_cast<int>(std::lround(d.treadDiscretizationType));
    const double halfTread = 0.5 * p.treadWidth;
    const double shoulder = std::clamp(d.relTreadShoulderWidth / 100.0, 0.0, 0.5) * p.treadWidth;
    const double inner = halfTread - shoulder;
    const double minShare = std::clamp(d.relMinTreadShoulderHeight / 100.0, 0.0, 1.0);
    const auto makeBlock = [&](double sigma, double lateral, int strip) {
        BlockLayout b;
        b.sigma = std::clamp(sigma, 0.0, 1.0);
        b.lateral = lateral;
        b.strip = strip;
        const double rl = p.latCurvatureRadius;
        b.crownDrop = rl > std::abs(lateral) ? -(rl - std::sqrt(rl * rl - lateral * lateral)) : -rl;
        const double a = std::abs(lateral);
        if (shoulder > 0.0 && a > inner)
        {
            const double t = std::clamp((a - inner) / shoulder, 0.0, 1.0);
            b.depthShare = 1.0 - (1.0 - minShare) * t * t;
        }
        return b;
    };
    if (type == 0)
    {
        // Herring-bone: each block its own sigma, zig-zagging across the strips.
        for (int i = 0; i < blocks; ++i)
        {
            int strip = 0;
            if (strips > 1)
            {
                const int period = 2 * (strips - 1);
                const int pos = i % period;
                strip = pos < strips ? pos : period - pos;
            }
            const double lateral = -halfTread + (strip + 0.5) * p.treadWidth / strips;
            p.blocks.push_back(makeBlock((i + 0.5) / blocks, lateral, strip));
        }
    }
    else
    {
        // Strips of equal block counts (FTire rounds the block count up to a multiple of the strips),
        // pseudo-randomly placed along the segment.
        const int perStrip = std::max((blocks + strips - 1) / strips, 1);
        blocks = perStrip * strips;
        for (int s = 0; s < strips; ++s)
        {
            const double lateral = -halfTread + (s + 0.5) * p.treadWidth / strips;
            for (int i = 0; i < perStrip; ++i)
            {
                const double sigma = (i + 0.5 + 0.6 * Jitter(s, i)) / perStrip;
                p.blocks.push_back(makeBlock(sigma, lateral, s));
            }
        }
    }
    p.blockArea = 2.0 * kPi * p.beltRadius * p.treadWidth / (n * static_cast<double>(p.blocks.size()));

    // Friction.
    FrictionTable& f = p.friction;
    f.pressure = {d.lowGroundPressure * 1.0e5, d.medGroundPressure * 1.0e5, d.highGroundPressure * 1.0e5};
    f.pressure[1] = std::max(f.pressure[1], f.pressure[0] + 1.0);
    f.pressure[2] = std::max(f.pressure[2], f.pressure[1] + 1.0);
    f.speed = {0.0, d.maxFrictionVelocity, d.slidingVelocity, d.blockingVelocity};
    f.speed[1] = std::max(f.speed[1], 1.0e-4);
    f.speed[2] = std::max(f.speed[2], f.speed[1] + 1.0e-4);
    f.speed[3] = std::max(f.speed[3], f.speed[2] + 1.0e-4);
    f.mu[0] = {d.muAdhesionAtLowP, d.muMaxAtLowP, d.muSlidingAtLowP, d.muBlockingAtLowP};
    f.mu[1] = {d.muAdhesionAtMedP, d.muMaxAtMedP, d.muSlidingAtMedP, d.muBlockingAtMedP};
    f.mu[2] = {d.muAdhesionAtHighP, d.muMaxAtHighP, d.muSlidingAtHighP, d.muBlockingAtHighP};

    // Numerics.
    p.maxStep = std::max(d.maximumTimeStep, 1.0e-6);
    p.maxAngleIncrement = std::max(d.maximumAngleIncrement, 0.01) * kDegree;
    p.beta = std::clamp(d.bdfParameter, 0.5, 1.0);
    p.jacobianCycle = std::max(static_cast<int>(std::lround(d.jacobianUpdateCycleLength)), 1);
    p.newtonIterations = std::max(static_cast<int>(std::lround(d.newtonIterations)), 1);
    p.contactBound = std::clamp(d.contactProcessorBound / 100.0, 0.01, 1.0);
    p.highPrecisionPlane = d.highPrecisionTangPlane >= 0.5;
    p.gravity = d.gravity >= 0.5;

    Derive(d, p);
    return p;
}

PreprocessResult Preprocess(const FlexRingData& d, const PreprocessOptions& options)
{
    const auto start = std::chrono::steady_clock::now();
    PreprocessResult result;
    result.parameters = BuildParameters(d);
    FlexRingParameters& p = result.parameters;
    const auto report = [&](const std::string& stage, double progress) {
        if (options.progress)
        {
            options.progress(stage, progress);
        }
    };
    const auto cancelled = [&] {
        return options.cancel != nullptr && options.cancel->load();
    };

    const double d1 = d.firstDeflection / 1000.0;
    const double d2 = d.secondDeflection / 1000.0;
    const double fz1 = d.statWheelLoadAtFirstDefl;
    const double fz2 = d.statWheelLoadAtSecondDefl;
    const bool staticOne = options.fitStatic && d1 > 0.0 && fz1 > 0.0;
    const bool staticTwo = staticOne && d2 > d1 && fz2 > 0.0;
    const bool fitMass = d.freeMassPercentage <= 0.0;
    const bool fitF5 = d.f5 > 0.0;
    const bool fitF6 = d.f6 > 0.0;
    const bool fitF3 = d.f3 > 0.0;
    const double progressMax = d.maxRadialProgressivity / 100.0;

    double staticFz1 = 0.0;
    double staticFz2 = 0.0;
    const auto measureStatic = [&] {
        FlexRingTyre tyre(p);
        FlatRoad road(0.0);
        std::vector<double> deflections = {d1};
        if (staticTwo)
        {
            deflections.push_back(d2);
        }
        const std::vector<StaticPoint> points = StaticDeflection(tyre, road, RigPose{}, deflections, 0.0025);
        staticFz1 = points[0].contact.normalForce;
        staticFz2 = staticTwo ? points[1].contact.normalForce : 0.0;
        bool ok = std::isfinite(staticFz1) && std::isfinite(staticFz2);
        for (const StaticPoint& pt : points)
        {
            ok = ok && pt.converged;
        }
        return ok;
    };

    Secant radialSecant;
    double progressSlope = 0.0;
    double lastProgress = 0.0;
    double lastRatio = 0.0;
    bool hasProgress = false;
    Secant bendInSecant;
    Secant bendOutSecant;
    Secant massSecant;
    // Targets the model cannot reach (a stiffness at its floor or a bound): no more cycles on them.
    bool f5Reachable = true;
    bool f6Reachable = true;
    bool f4Reachable = true;
    bool secondLoadReachable = true;

    for (int cycle = 0; cycle < options.maxCycles && !cancelled(); ++cycle)
    {
        result.cycles = cycle + 1;
        bool changed = false;
        // Static: the radial stiffness for the first load, the progressivity for the second.
        if (staticOne)
        {
            double goodRadial = p.radialStiffness;
            double goodProgress = p.radialProgressivity;
            for (int it = 0; it < 16 && !cancelled(); ++it)
            {
                report("Static fit", (cycle + it / 16.0 * 0.5) / options.maxCycles);
                if (!measureStatic())
                {
                    // A static solve that did not settle: back half way to the last good values.
                    p.radialStiffness = std::sqrt(p.radialStiffness * goodRadial);
                    p.radialProgressivity = 0.5 * (p.radialProgressivity + goodProgress);
                    radialSecant = Secant{};
                    hasProgress = false;
                    Derive(d, p);
                    changed = true;
                    continue;
                }
                goodRadial = p.radialStiffness;
                goodProgress = p.radialProgressivity;
                const double e1 = staticFz1 / fz1 - 1.0;
                const double ratioTarget = staticTwo ? fz2 / fz1 : 0.0;
                const double ratio = staticTwo && staticFz1 > 0.0 ? staticFz2 / staticFz1 : 0.0;
                const double e2 = staticTwo && secondLoadReachable ? ratio / ratioTarget - 1.0 : 0.0;
                if (std::abs(e1) < options.tolerance && std::abs(e2) < options.tolerance)
                {
                    break;
                }
                changed = true;
                if (std::abs(e1) >= options.tolerance)
                {
                    p.radialStiffness = radialSecant.Next(p.radialStiffness, std::max(staticFz1, 1.0), fz1, 0.7);
                }
                if (staticTwo && std::abs(e2) >= options.tolerance)
                {
                    double slope = 0.3;
                    if (hasProgress && std::abs(p.radialProgressivity - lastProgress) > 1.0e-9)
                    {
                        const double sl = (ratio - lastRatio) / (p.radialProgressivity - lastProgress);
                        if (sl > 1.0e-3)
                        {
                            slope = sl;
                        }
                    }
                    progressSlope = slope;
                    lastProgress = p.radialProgressivity;
                    lastRatio = ratio;
                    hasProgress = true;
                    const double step = std::clamp((ratioTarget - ratio) / slope, -0.3, 0.3);
                    const double before = p.radialProgressivity;
                    p.radialProgressivity = std::clamp(p.radialProgressivity + step, -0.9, std::max(progressMax, 0.0));
                    if (p.radialProgressivity == before)
                    {
                        secondLoadReachable = false;
                    }
                }
                Derive(d, p);
            }
        }
        if (cancelled())
        {
            break;
        }
        // Modal: the six reference frequencies of the unloaded tyre.
        if (options.fitModal)
        {
            for (int it = 0; it < 16 && !cancelled(); ++it)
            {
                report("Modal fit", (cycle + 0.5 + it / 16.0 * 0.5) / options.maxCycles);
                FlexRingTyre tyre(p);
                const ModalResult modal = AnalyzeUnloadedModes(tyre, 3);
                result.modal = modal;
                const double m = p.nodeMass;
                const double r = p.beltRadius;
                const double j = p.nodeInertia;
                const auto off = [&](double achieved, double target) {
                    return target > 0.0 && achieved > 0.0 ? std::abs(achieved / target - 1.0) : 0.0;
                };
                const double err = std::max({off(modal.f1, d.f1), fitMass ? off(modal.f2, d.f2) : 0.0, fitF3 ? off(modal.f3, d.f3) : 0.0, f4Reachable ? off(modal.f4, d.f4) : 0.0,
                                             fitF5 && f5Reachable ? off(modal.f5, d.f5) : 0.0, fitF6 && f6Reachable ? off(modal.f6, d.f6) : 0.0});
                if (err < options.tolerance)
                {
                    break;
                }
                changed = true;
                if (fitMass && modal.f2 > 0.0)
                {
                    // w2^2 ~ (c_r + c_t) / 2m: the mass for f2, the other stiffnesses scaled along.
                    const double newMass = massSecant.Next(m, 1.0 / (modal.f2 * modal.f2), 1.0 / (d.f2 * d.f2), 1.0);
                    const double scale = newMass / m;
                    p.nodeMass = newMass;
                    p.tangentialStiffness *= scale;
                    p.lateralStiffness *= scale;
                    p.torsionStiffness *= scale;
                    p.rimFixedMass = d.tireMass - newMass * p.segments;
                }
                {
                    if (modal.f1 > 0.0)
                    {
                        p.tangentialStiffness *= (d.f1 / modal.f1) * (d.f1 / modal.f1);
                    }
                    const double w4t = Omega(d.f4);
                    const double w4 = Omega(modal.f4);
                    if (fitF3 && modal.f3 > 0.0)
                    {
                        p.lateralStiffness *= (d.f3 / modal.f3) * (d.f3 / modal.f3);
                        if (modal.f4 > 0.0 && f4Reachable)
                        {
                            p.torsionStiffness = std::max(p.torsionStiffness + (m * r * r + j) * (w4t * w4t - w4 * w4), 1.0e-6);
                            f4Reachable = !(p.torsionStiffness <= 1.0e-6 && modal.f4 > d.f4);
                        }
                    }
                    else if (modal.f4 > 0.0)
                    {
                        p.lateralStiffness = std::max(p.lateralStiffness + (m * r * r + j) * (w4t * w4t - w4 * w4) / (r * r), 1.0);
                    }
                    const double chord3 = p.chord * p.chord * p.chord;
                    if (fitF5 && f5Reachable && modal.f5 > 0.0)
                    {
                        p.bendInStiffness = std::max(bendInSecant.Next(std::max(p.bendInStiffness, 1.0e-9), modal.f5, d.f5, 0.3, 1.0e-3), 1.0e-4 / chord3);
                        f5Reachable = !(p.bendInStiffness * chord3 <= 1.0001e-4 && modal.f5 > d.f5);
                    }
                    if (fitF6 && f6Reachable && modal.f6 > 0.0)
                    {
                        p.bendOutStiffness = std::min(bendOutSecant.Next(std::max(p.bendOutStiffness, 1.0e-9), modal.f6, d.f6, 0.3, 1.0e-3), 1.0e5 / chord3);
                        f6Reachable = !(p.bendOutStiffness * chord3 >= 0.9999e5 && modal.f6 < d.f6);
                    }
                }
                Derive(d, p);
            }
        }
        if (!changed)
        {
            result.converged = true;
            break;
        }
    }
    (void)progressSlope;

    // What the fitted model shows.
    report("Checking", 0.97);
    FlexRingTyre tyre(p);
    result.modal = AnalyzeUnloadedModes(tyre, 4);
    if (staticOne)
    {
        measureStatic();
    }
    const auto add = [&](const std::string& name, const std::string& unit, double target, double achieved, bool fitted) {
        FitTarget t;
        t.name = name;
        t.unit = unit;
        t.target = target;
        t.achieved = achieved;
        t.fitted = fitted;
        result.targets.push_back(t);
    };
    if (staticOne)
    {
        add("stat_wheel_load_at_first_defl", "N", fz1, staticFz1, true);
    }
    if (staticTwo)
    {
        add("stat_wheel_load_at_second_defl", "N", fz2, staticFz2, true);
    }
    add("f1", "Hz", d.f1, result.modal.f1, options.fitModal);
    add("f2", "Hz", d.f2, result.modal.f2, options.fitModal && fitMass);
    if (fitF3)
    {
        add("f3", "Hz", d.f3, result.modal.f3, options.fitModal);
    }
    add("f4", "Hz", d.f4, result.modal.f4, options.fitModal);
    add("f5", "Hz", d.f5, result.modal.f5, options.fitModal && fitF5);
    add("f6", "Hz", d.f6, result.modal.f6, options.fitModal && fitF6);
    add("D1", "-", d.d1, result.modal.d1, false);
    add("D2", "-", d.d2, result.modal.d2, false);
    add("D4", "-", d.d4, result.modal.d4, false);

    const double chord3 = p.chord * p.chord * p.chord;
    std::ostringstream notes;
    result.freeMass = p.nodeMass * p.segments;
    result.beltRadius = p.beltRadius;
    result.outerRadius = p.outerRadius;
    if (fitMass && (result.freeMass > d.tireMass || result.freeMass < 0.2 * d.tireMass))
    {
        result.notes.push_back("The belt mass fitted to f2 (" + std::to_string(result.freeMass) + " kg) is outside 20..100 % of the tyre mass; give free_mass_percentage.");
    }
    if (fitF5 && !f5Reachable)
    {
        result.notes.push_back("f5 is above the target with no in-plane bending stiffness: inflation pressure and the radial and tangential foundation alone set it.");
    }
    if (fitF6 && !f6Reachable)
    {
        result.notes.push_back("f6 stays below the target at the largest out-of-plane bending stiffness: the belt's torsion and twist stiffness limit it.");
    }
    if (!f4Reachable)
    {
        result.notes.push_back("f4 is above the target with no torsion stiffness to the rim: the lateral stiffness (f3) and the twist stiffness set it.");
    }
    if (staticTwo && (p.radialProgressivity <= -0.9 + 1.0e-9 || p.radialProgressivity >= progressMax - 1.0e-9))
    {
        result.notes.push_back("The radial progressivity hit its bound; the second static load is not met exactly.");
    }
    bool all = true;
    for (const FitTarget& t : result.targets)
    {
        if (t.fitted && t.target > 0.0 && std::abs(t.RelativeError()) > 10.0 * options.tolerance)
        {
            all = false;
        }
    }
    result.converged = result.converged || all;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    report("Done", 1.0);
    return result;
}

}
