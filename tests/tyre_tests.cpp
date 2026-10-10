#include <engine/tyre/tyre_brush.h>
#include <engine/tyre/tyre_thermal.h>
#include <engine/tyre/tyre_wear.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace me::tyre;

namespace
{
constexpr double kPi = 3.14159265358979323846;
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

// ---- The brush tyre with a flexible carcass ----

// A wheel rolling at 20 m/s on the brush tyre at a load, with sideways velocity for a slip angle
// (alpha = atan(V_y / V_x)) and the wheel's spin for a theoretical longitudinal slip (V_r - V_x) / V_r.
BrushTyreInput Rolling(const BrushTyreParameters& p, double load, double alpha, double slip = 0.0, double speed = 20.0)
{
    BrushTyreInput in;
    in.load = load;
    in.forwardVelocity = speed;
    in.lateralVelocity = std::abs(speed) * std::tan(alpha);
    const double radius = p.unloadedRadius - load / p.verticalRate / 3.0;
    in.wheelSpeed = speed / (1.0 - slip) / radius;
    return in;
}

// A tyre's figures with only its grip, peak, size and rate given.
BrushTyreFigures Figures(double mu, double load, double peakSlipAngle, double radius, double width, double verticalRate)
{
    BrushTyreFigures f;
    f.peakFriction = mu;
    f.referenceLoad = load;
    f.peakSlipAngle = peakSlipAngle;
    f.kineticShare = 0.85;
    f.radius = radius;
    f.sectionWidth = width;
    f.verticalRate = verticalRate;
    return f;
}

// The distance rolled at 20 m/s until a step in slip has built 63% of its steady force, along the wheel
// (slip ratio) or across it (slip angle).
double RelaxationDistance(const BrushTyreParameters& p, double load, bool lateral)
{
    BrushTyre tyre(p);
    const BrushTyreInput in = lateral ? Rolling(p, load, 1.0 * kDeg) : Rolling(p, load, 0.0, 0.01);
    const BrushTyreOutput steady = tyre.Steady(in);
    const double target = 0.632 * std::abs(lateral ? steady.Fy : steady.Fx);
    constexpr double kStep = 1e-4;
    for (int step = 1; step <= 5000; ++step)
    {
        const BrushTyreOutput o = tyre.Step(in, kStep);
        if (std::abs(lateral ? o.Fy : o.Fx) >= target)
        {
            return step * kStep * 20.0;
        }
    }
    return -1.0;
}

// The patch from the inflation pressure and the tread less its shoulders: Pacejka (2006, Table 9.1)
// measures a 205/60R15 at 4 kN and 2.2 bar with a 107 mm patch (a = 0.0535 m); its vertical stiffness
// is q_Fz1 Fz0 / R0 of Table A3.1.
void TestBrushPatchFollowsTheInflationPressure()
{
    BrushTyreFigures f = Figures(1.0, 4000.0, 6.0 * kDeg, 0.313, 0.205, 13.37 * 4000.0 / 0.313);
    f.rimRadius = 0.313 - 0.205 * 0.6;
    f.inflationPressure = 2.2e5;
    const BrushTyreParameters p = MakeBrushTyreParameters(f);
    RequireNear(p.width, 0.205 - 2.0 * 0.15 * 0.123, 1e-9, "the tread is the section less its shoulders");
    const BrushTyreOutput o = BrushTyre(p).Steady(Rolling(p, 4000.0, 0.0));
    std::cout << "  brush: 205/60R15 at 4 kN, 2.2 bar: contact length " << o.contactLength * 1000.0 << " mm (measured 107)\n";
    RequireNear(o.contactLength, 0.107, 0.005, "contact length against Pacejka's measured patch");
    RequireNear(o.contactLength * p.width, 4000.0 / 2.2e5, 1e-6, "mean contact pressure is the inflation pressure");
    // More load lengthens the patch, more pressure shortens it.
    Require(BrushTyre(p).Steady(Rolling(p, 6000.0, 0.0)).contactLength > o.contactLength, "a longer patch under more load");
    f.inflationPressure = 3.0e5;
    const BrushTyreParameters firmer = MakeBrushTyreParameters(f);
    Require(BrushTyre(firmer).Steady(Rolling(firmer, 4000.0, 0.0)).contactLength < o.contactLength, "a shorter patch at more pressure");
}

// The carcass is stiffened to the data's relaxation length both ways, rolling; the tread's fore-aft
// stiffness is the data's multiple of its sideways one.
void TestBrushRelaxesOverTheGivenLength()
{
    for (double length : {0.0757, 0.2})
    {
        BrushTyreFigures f = Figures(1.2, 4200.0, 8.0 * kDeg, 0.3266, 0.245, 325000.0);
        f.rimRadius = 0.254;
        f.inflationPressure = 1.93e5;
        f.relaxationLength = length;
        f.longitudinalStiffnessRatio = 1.04;
        const BrushTyreParameters p = MakeBrushTyreParameters(f);
        const auto stiffness = BrushTyre(p).BristleSlipStiffness(4200.0);
        RequireNear(stiffness[0] / stiffness[1], 1.04, 1e-9, "fore-aft over sideways stiffness");
        const double lateral = RelaxationDistance(p, 4200.0, true);
        const double longitudinal = RelaxationDistance(p, 4200.0, false);
        std::cout << "  brush: relaxation length " << length << " m asked, " << lateral << " m sideways, " << longitudinal << " m fore and aft\n";
        RequireNear(lateral, length, 0.03 * length, "sideways relaxation length");
        RequireNear(longitudinal, length, 0.03 * length, "fore-aft relaxation length");
    }
}

// One rib, a parabolic pressure (lambda = 12), one friction coefficient and a carcass too stiff to move:
// the classic brush, whose force Pacejka (2006, 3.2.1-3.2.2) gives in closed form,
//   F = 3 mu Fz theta s (1 - |theta s| + (theta s)^2 / 3), theta = 2 c a^2 / (3 mu Fz),
// up to full sliding at |theta s| = 1, with c the bristles' stiffness per unit length and a half the patch.
void TestRigidBrushMatchesTheClosedForm()
{
    BrushTyreParameters p;
    p.ribs = 1;
    p.segmentsPerRib = 400;
    p.pressureConvexity = 12.0;
    p.pressureShift = 0.0;
    p.kineticShare = 1.0;
    p.loadExponent = {1.0, 1.0};
    p.lowSpeed = 1e-4;
    p.carcassStiffness = {1e12, 1e12, 1e12};
    p.camberSpinShare = 0.0;
    const BrushTyre tyre(p);
    const double load = 4000.0;
    const double deflection = load / p.verticalRate;
    const double a = std::sqrt((2.0 * (p.unloadedRadius - p.transitionRadius) - deflection) * deflection);
    const double theta = 2.0 * p.bristleStiffnessY * p.width * a * a / (3.0 * p.staticFriction[1] * load);
    const auto closed = [&](double s)
    {
        const double ts = std::min(std::abs(theta * s), 1.0);
        const double f = std::abs(theta * s) >= 1.0 ? 1.0 : 3.0 * ts * (1.0 - ts + ts * ts / 3.0);
        return p.staticFriction[1] * load * f;
    };
    for (double degrees : {0.5, 1.0, 2.0, 3.0, 5.0, 10.0})
    {
        const double s = std::tan(degrees * kDeg);
        const BrushTyreOutput o = tyre.Steady(Rolling(p, load, degrees * kDeg));
        RequireNear(o.Fy, -closed(s), 3e-3 * closed(s) + 1.0, "rigid brush Fy at " + std::to_string(degrees) + " deg");
    }
    for (double slip : {0.01, 0.03, 0.08, 0.2})
    {
        const BrushTyreOutput o = tyre.Steady(Rolling(p, load, 0.0, slip));
        RequireNear(o.Fx, closed(slip) * p.bristleStiffnessX / p.bristleStiffnessY, 3e-3 * closed(slip) + 1.0, "rigid brush Fx at slip " + std::to_string(slip));
    }
}

// The road's force never passes the friction ellipse, whatever the slips (local friction is at most mu_s
// each way), on a tyre with one friction and on one gripping more along the wheel than across it.
void TestBrushStaysInsideTheFrictionEllipse()
{
    BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    for (double along : {0.0, 1.3})
    {
        figures.longitudinalPeakFriction = along;
        const BrushTyreParameters p = MakeBrushTyreParameters(figures);
        const BrushTyre tyre(p);
        for (double slip : {-0.6, -0.15, -0.04, 0.0, 0.03, 0.1, 0.5})
        {
            for (double degrees : {-20.0, -6.0, -1.0, 0.0, 2.0, 8.0, 25.0})
            {
                for (double camber : {-3.0, 0.0, 4.0})
                {
                    BrushTyreInput in = Rolling(p, 4000.0, degrees * kDeg, slip);
                    in.camber = camber * kDeg;
                    const BrushTyreOutput o = tyre.Steady(in);
                    Require(o.converged, "steady balance converges at slip " + std::to_string(slip) + ", " + std::to_string(degrees) + " deg");
                    Require(std::hypot(o.Fx / o.peakFrictionX, o.Fy / o.peakFrictionY) <= 4000.0 * 1.001,
                            "inside the friction ellipse at slip " + std::to_string(slip) + ", " + std::to_string(degrees) + " deg");
                }
            }
        }
    }
}

// The fitted tyre peaks where it was asked to, and with the sign conventions of the ISO axes: a slip
// angle to the left pushes right with an aligning moment, a lean to the right pushes right, driving slip
// pushes forward; the flexible carcass softens the bristles' cornering stiffness.
void TestFittedBrushPeaksWhereAskedAndPointsTheRightWay()
{
    for (double peak : {4.0, 7.0, 10.0})
    {
        const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.2, 3500.0, peak * kDeg, 0.33, 0.25, 0.0));
        const BrushTyre tyre(p);
        double best = 0.0;
        double bestAngle = 0.0;
        for (double degrees = 0.25; degrees <= 20.0; degrees += 0.25)
        {
            const double fy = std::abs(tyre.Steady(Rolling(p, 3500.0, degrees * kDeg)).Fy);
            if (fy > best)
            {
                best = fy;
                bestAngle = degrees;
            }
        }
        RequireNear(bestAngle, peak, 0.6, "lateral peak angle");
        Require(best > 0.85 * 1.2 * 3500.0 && best <= 1.2 * 3500.0, "the peak is near mu Fz, got " + std::to_string(best));
    }
    const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    const BrushTyre tyre(p);
    const BrushTyreOutput left = tyre.Steady(Rolling(p, 4000.0, 2.0 * kDeg));
    Require(left.Fy < 0.0 && left.Mz > 0.0, "a slip angle to the left pushes right and aligns");
    const BrushTyreOutput right = tyre.Steady(Rolling(p, 4000.0, -2.0 * kDeg));
    RequireNear(right.Fy, -left.Fy, 1e-6 * std::abs(left.Fy) + 1e-3, "symmetric in slip angle (no pressure shift)");
    RequireNear(right.Mz, -left.Mz, 1e-6 * std::abs(left.Mz) + 1e-3, "aligning moment symmetric");
    BrushTyreInput leaning = Rolling(p, 4000.0, 0.0);
    leaning.camber = 3.0 * kDeg;
    Require(tyre.Steady(leaning).Fy < 0.0, "a lean to the right pushes right (camber thrust)");
    Require(tyre.Steady(Rolling(p, 4000.0, 0.0, 0.05)).Fx > 0.0, "driving slip pushes forward");
    Require(tyre.Steady(Rolling(p, 4000.0, 0.0, -0.05)).Fx < 0.0, "braking slip pushes back");

    // The carcass's twist and bend add to the slip: the cornering stiffness falls below the bristles'.
    const double small = 0.2 * kDeg;
    const double flexible = std::abs(tyre.Steady(Rolling(p, 4000.0, small)).Fy) / std::tan(small);
    const double bristles = tyre.BristleSlipStiffness(4000.0)[1];
    Require(flexible < 0.97 * bristles && flexible > 0.5 * bristles,
            "carcass softens the cornering stiffness: " + std::to_string(flexible) + " against " + std::to_string(bristles));

    // Rolling backwards it pushes the same way against the same sideways motion, its moment turned round.
    const BrushTyreOutput forward = tyre.Steady(Rolling(p, 4000.0, 0.0, 0.0, 5.0));
    BrushTyreInput sideways = Rolling(p, 4000.0, 0.0, 0.0, 5.0);
    sideways.lateralVelocity = 0.2;
    BrushTyreInput backwards = Rolling(p, 4000.0, 0.0, 0.0, -5.0);
    backwards.lateralVelocity = 0.2;
    const BrushTyreOutput ahead = tyre.Steady(sideways);
    const BrushTyreOutput behind = tyre.Steady(backwards);
    Require(std::abs(forward.Fy) < 1e-6, "no side force rolling straight");
    RequireNear(behind.Fy, ahead.Fy, 1e-3 * std::abs(ahead.Fy), "same side force rolling backwards");
    Require(behind.Mz * ahead.Mz < 0.0, "the aligning moment turns round rolling backwards");
}

// A step in slip angle: the force builds over the carcass's relaxation length, not at once.
void TestBrushForceBuildsOverItsRelaxationLength()
{
    const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    BrushTyre tyre(p);
    const BrushTyreInput in = Rolling(p, 4000.0, 2.0 * kDeg);
    const double steady = tyre.Steady(in).Fy;
    double previous = 0.0;
    double distanceTo63 = -1.0;
    int evaluations = 0;
    for (int step = 1; step <= 200; ++step)
    {
        const BrushTyreOutput o = tyre.Step(in, 1e-3);
        evaluations += o.evaluations;
        Require(o.converged, "each step's balance converges");
        Require(std::abs(o.Fy) >= std::abs(previous) - 1e-5 * std::abs(steady), "the force builds without overshoot");
        if (step == 1)
        {
            Require(std::abs(o.Fy) < 0.2 * std::abs(steady), "not all at once: first millisecond " + std::to_string(o.Fy / steady));
        }
        if (distanceTo63 < 0.0 && std::abs(o.Fy) >= 0.632 * std::abs(steady))
        {
            distanceTo63 = step * 1e-3 * 20.0;
        }
        previous = o.Fy;
    }
    RequireNear(previous, steady, 1e-3 * std::abs(steady), "settles on the steady force");
    std::cout << "  brush: 63% of a slip-angle step after " << distanceTo63 << " m, " << evaluations / 200.0 << " evaluations per step\n";
    Require(distanceTo63 > 0.02 && distanceTo63 < 0.6, "a relaxation length of centimetres to decimetres, got " + std::to_string(distanceTo63) + " m");
}

// Standing still the tyre makes no force and does not drift. Pushed a few millimetres it holds like a
// spring within friction, and held there it keeps holding: the bristles keep their bend, so nothing
// creeps. Pushed on past friction it slides and, stopped, holds what friction leaves.
void TestBrushStandsAndHolds()
{
    const BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    BrushTyre tyre(p);
    BrushTyreInput still;
    still.load = 4000.0;
    for (int step = 0; step < 2000; ++step)
    {
        const BrushTyreOutput o = tyre.Step(still, 1e-3);
        Require(o.Fx == 0.0 && o.Fy == 0.0, "no force standing still");
    }
    BrushTyreInput pushed = still;
    pushed.forwardVelocity = 0.01;
    pushed.lateralVelocity = 0.005;
    double most = 0.0;
    for (int step = 0; step < 500; ++step)
    {
        const BrushTyreOutput o = tyre.Step(pushed, 1e-3);
        Require(std::isfinite(o.Fx) && std::isfinite(o.Fy), "finite");
        most = std::max(most, std::hypot(o.Fx, o.Fy));
        Require(std::hypot(o.Fx / o.peakFrictionX, o.Fy / o.peakFrictionY) <= 4000.0 * 1.001, "within friction while pushed");
    }
    Require(most > 500.0, "a 5 mm push is held by a spring's force, got " + std::to_string(most) + " N");
    const BrushTyreOutput held = tyre.Step(still, 1e-3);
    Require(held.Fx < 0.0 && held.Fy < 0.0, "the force resists the push");
    BrushTyreOutput later = held;
    for (int step = 0; step < 3000; ++step)
    {
        later = tyre.Step(still, 1e-3);
    }
    std::cout << "  brush: held after a 5 mm push " << std::hypot(held.Fx, held.Fy) << " N, 3 s later " << std::hypot(later.Fx, later.Fy) << " N\n";
    RequireNear(std::hypot(later.Fx, later.Fy), std::hypot(held.Fx, held.Fy), 0.05 * std::hypot(held.Fx, held.Fy), "held still, the force stays: no creep");

    // Dragged 10 cm sideways the tread slides at friction; stopped, it holds no more than friction.
    BrushTyreInput dragged = still;
    dragged.lateralVelocity = 0.2;
    BrushTyreOutput sliding;
    for (int step = 0; step < 500; ++step)
    {
        sliding = tyre.Step(dragged, 1e-3);
    }
    Require(sliding.slidingShare > 0.9, "dragged, the tread slides: " + std::to_string(sliding.slidingShare));
    RequireNear(-sliding.Fy, sliding.peakFrictionY * 4000.0, 0.1 * sliding.peakFrictionY * 4000.0, "at about friction");
    BrushTyreOutput stopped;
    for (int step = 0; step < 1000; ++step)
    {
        stopped = tyre.Step(still, 1e-3);
    }
    Require(std::hypot(stopped.Fx / stopped.peakFrictionX, stopped.Fy / stopped.peakFrictionY) <= 4000.0 * 1.001, "stopped, within friction");
    Require(-stopped.Fy > 0.5 * stopped.peakFrictionY * 4000.0, "and still holding what the slide bent: " + std::to_string(stopped.Fy));
}

// A hard landing hands the tyre one step of some 8 MN: its tread squashes only to the rim, so the rolling
// radius stays the tyre's and the force on a rolling wheel still opposes its slip (it once read 24 m of
// deflection, a radius of -7.8 m, and the wheel's torque flung it backwards).
void TestBrushSquashesNoFurtherThanTheRim()
{
    BrushTyreParameters p = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.3266, 0.245, 325354.0));
    p.rimDeflection = 0.3266 - 0.254 - 0.015;
    BrushTyre tyre(p);
    BrushTyreInput in;
    in.forwardVelocity = -0.3;
    in.wheelSpeed = 2.5;
    in.load = 4000.0;
    tyre.Step(in, 1e-3);
    in.load = 7.9e6;
    const BrushTyreOutput o = tyre.Step(in, 1e-3);
    RequireNear(o.effectiveRadius, 0.3266 - p.rimDeflection / 3.0, 1e-9, "the rolling radius at the rim's contact");
    Require(std::isfinite(o.Fx) && o.Fx > 0.0, "the tread turning faster than the road drives the car: " + std::to_string(o.Fx));
    Require(-o.Fx * o.effectiveRadius < 0.0, "and the wheel's torque slows it");

    // Without a rim the radius is still held above R0 less the belt's transition radius.
    BrushTyre bare(MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.3266, 0.245, 325354.0)));
    Require(bare.Step(in, 1e-3).effectiveRadius > 0.0, "a tyre without a rim keeps a positive radius");
}

// Friction along the wheel apart from across it: without any fall from sliding, the tread sliding
// throughout pushes with each friction along its own axis, and on the ellipse between them.
void TestBrushGripsEachWayItsOwn()
{
    BrushTyreFigures figures = Figures(1.0, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    figures.longitudinalPeakFriction = 1.3;
    figures.kineticShare = 1.0;
    figures.longitudinalStiffnessRatio = 1.0;
    BrushTyreParameters p = MakeBrushTyreParameters(figures);
    p.loadExponent = {1.0, 1.0};
    const BrushTyre tyre(p);
    const BrushTyreOutput along = tyre.Steady(Rolling(p, 4000.0, 0.0, 0.5));
    const BrushTyreOutput across = tyre.Steady(Rolling(p, 4000.0, 25.0 * kDeg));
    BrushTyreInput both = Rolling(p, 4000.0, 0.0, 0.3);
    both.lateralVelocity = 0.5 * (both.wheelSpeed * (p.unloadedRadius - 4000.0 / p.verticalRate / 3.0) - both.forwardVelocity);
    const BrushTyreOutput combined = tyre.Steady(both);
    std::cout << "  brush: mu 1.3 along, 1.0 across: sliding Fx/Fz " << along.Fx / 4000.0 << ", Fy/Fz " << -across.Fy / 4000.0 << ", combined on the ellipse at "
              << std::hypot(combined.Fx / 1.3, combined.Fy / 1.0) / 4000.0 << "\n";
    RequireNear(along.Fx, 1.3 * 4000.0, 0.01 * 1.3 * 4000.0, "along the wheel, its own friction");
    RequireNear(-across.Fy, 1.0 * 4000.0, 0.01 * 4000.0, "across it, its own");
    RequireNear(std::hypot(combined.Fx / 1.3, combined.Fy / 1.0), 4000.0, 0.01 * 4000.0, "between them, the ellipse");
    Require(combined.Fx > 0.0 && combined.Fy < 0.0, "pushing against the sliding");
}

// Without the data's stiffness ratio the tread's fore-aft stiffness puts the longitudinal peak at the
// slip ratio asked for; with it, the ratio stands. (At 20 m/s the sliding friction's fall holds the
// peak below about 0.2 however soft the tread.)
void TestBrushPeaksAtTheSlipRatioAskedFor()
{
    for (double asked : {0.06, 0.1, 0.15})
    {
        BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
        figures.peakSlipRatio = asked;
        const BrushTyreParameters p = MakeBrushTyreParameters(figures);
        const BrushTyre tyre(p);
        const double radius = p.unloadedRadius - 4000.0 / p.verticalRate / 3.0;
        double best = 0.0;
        double bestRatio = 0.0;
        for (double ratio = 0.002; ratio <= 0.8; ratio += 0.002)
        {
            BrushTyreInput in = Rolling(p, 4000.0, 0.0);
            in.wheelSpeed = 20.0 * (1.0 + ratio) / radius;
            const double fx = tyre.Steady(in).Fx;
            if (fx > best)
            {
                best = fx;
                bestRatio = ratio;
            }
        }
        std::cout << "  brush: longitudinal peak asked at " << asked << ", got " << bestRatio << " (fore-aft over sideways stiffness "
                  << p.bristleStiffnessX / p.bristleStiffnessY << ")\n";
        RequireNear(bestRatio, asked, 0.04 * asked + 0.004, "the longitudinal peak's slip ratio");
    }
    BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    figures.peakSlipRatio = 0.08;
    figures.longitudinalStiffnessRatio = 1.04;
    const BrushTyreParameters p = MakeBrushTyreParameters(figures);
    RequireNear(p.bristleStiffnessX / p.bristleStiffnessY, 1.04, 1e-9, "the data's stiffness ratio stands");
}

// A tyre that loses no grip sliding has no peak to put at the angle asked for: it is fitted to reach its
// limit there instead, and its bristles stay those of a tyre that does fall.
void TestBrushWithoutFalloffReachesItsLimitWhereAsked()
{
    BrushTyreFigures figures = Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0);
    const BrushTyreParameters falls = MakeBrushTyreParameters(figures);
    figures.kineticShare = 1.0;
    const BrushTyreParameters p = MakeBrushTyreParameters(figures);
    const BrushTyre tyre(p);
    double limit = 0.0;
    for (double degrees = 0.1; degrees <= 25.0; degrees += 0.1)
    {
        limit = std::max(limit, std::abs(tyre.Steady(Rolling(p, 4000.0, degrees * kDeg)).Fy));
    }
    double reached = 0.0;
    for (double degrees = 0.1; degrees <= 25.0; degrees += 0.1)
    {
        if (std::abs(tyre.Steady(Rolling(p, 4000.0, degrees * kDeg)).Fy) >= 0.999 * limit)
        {
            reached = degrees;
            break;
        }
    }
    std::cout << "  brush without fall: at its limit from " << reached << " deg (asked 7), bristles " << p.bristleStiffnessY / falls.bristleStiffnessY
              << " times a falling tyre's\n";
    RequireNear(reached, 7.0, 0.6, "reaches its limit at the angle asked for");
    Require(p.bristleStiffnessY < 2.0 * falls.bristleStiffnessY && p.bristleStiffnessY > 0.5 * falls.bristleStiffnessY,
            "bristles near a falling tyre's, not inflated");
}
}

// The finest cut the tyre takes (kBrushMaxRibs x kBrushMaxSegments) steps to its own steady state and to
// about the default cut's forces; report what a step costs at each cut.
void TestFinelyCutBrushAgreesWithTheDefault()
{
    const BrushTyreParameters coarse = MakeBrushTyreParameters(Figures(1.1, 4000.0, 7.0 * kDeg, 0.32, 0.225, 250000.0));
    BrushTyreParameters fine = coarse;
    fine.ribs = kBrushMaxRibs;
    fine.segmentsPerRib = kBrushMaxSegments;
    const double limit = 1.1 * 4000.0;
    for (const auto& [degrees, slip] : {std::pair{2.0, 0.0}, std::pair{6.0, 0.03}, std::pair{12.0, -0.05}})
    {
        const BrushTyreInput in = Rolling(coarse, 4000.0, degrees * kDeg, slip);
        BrushTyre stepped(fine);
        BrushTyreOutput o;
        for (int step = 0; step < 300; ++step)
        {
            o = stepped.Step(in, 1e-3);
        }
        const std::string where = std::to_string(degrees) + " deg, slip " + std::to_string(slip);
        Require(o.converged && o.ribCount == kBrushMaxRibs, "finest cut converges at " + where);
        const BrushTyreOutput steady = BrushTyre(fine).Steady(in);
        RequireNear(o.Fx, steady.Fx, 2e-3 * limit, "finest cut stepped Fx = steady at " + where);
        RequireNear(o.Fy, steady.Fy, 2e-3 * limit, "finest cut stepped Fy = steady at " + where);
        const BrushTyreOutput usual = BrushTyre(coarse).Steady(in);
        RequireNear(steady.Fx, usual.Fx, 0.02 * limit, "finest cut Fx near the default's at " + where);
        RequireNear(steady.Fy, usual.Fy, 0.02 * limit, "finest cut Fy near the default's at " + where);
        std::cout << "  brush " << where << ": Fy default " << usual.Fy << " N, finest " << steady.Fy << " N; Fx " << usual.Fx << " / " << steady.Fx << " N\n";
    }
    for (const auto& [ribs, segments] : {std::pair{10, 20}, std::pair{32, 32}, std::pair{100, 20}, std::pair{64, 64}, std::pair{128, 128}})
    {
        BrushTyreParameters p = coarse;
        p.ribs = ribs;
        p.segmentsPerRib = segments;
        BrushTyre tyre(p);
        const BrushTyreInput in = Rolling(p, 4000.0, 4.0 * kDeg, 0.02);
        constexpr int kSteps = 200;
        const auto start = std::chrono::steady_clock::now();
        for (int step = 0; step < kSteps; ++step)
        {
            tyre.Step(in, 1e-3);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "  brush " << ribs << " x " << segments << ": " << seconds / kSteps * 1e6 << " us per step\n";
    }
}

// The RX-7 Tuned's front semislicks as tyres.ini gives them.
TyreThermalParameters Rx7Semislicks()
{
    TyreThermalParameters p;
    p.surfaceTransfer = 0.0150;
    p.patchTransfer = 0.00027;
    p.coreTransfer = 0.00015;
    p.internalCoreTransfer = 0.0029;
    p.frictionK = 0.06446;
    p.rollingK = 0.18;
    p.surfaceRollingK = 0.96443;
    p.coolFactor = 2.17;
    p.performanceCurve = {{0, 0.8f}, {20, 0.92f}, {40, 0.95f}, {60, 0.98f}, {75, 1.0f}, {85, 1.0f}, {95, 1.0f}, {105, 0.97f}, {140, 0.95f}};
    p.staticPressure = 28.0;
    p.idealPressure = 33.0;
    p.rollingResistanceGain = 0.55;
    return p;
}

// Each part of the game's thermal model alone, one step from rest, against its formula.
void TestThermalModelStepsAsTheGame()
{
    const double dt = 0.001;
    const double load = 3000.0;
    const double spin = 90.0;
    // Cold: everything at 26 C, the pressure cold, the grip the curve's at 26 C.
    {
        const TyreThermalModel cold(Rx7Semislicks());
        RequireNear(cold.CoreTemperature(), 26.0, 0.0, "starts at the air's temperature");
        RequireNear(cold.Pressure(), 28.0, 1e-12, "at the cold pressure");
        RequireNear(cold.Performance(), 0.92 + (0.95 - 0.92) * 6.0 / 20.0, 1e-6, "with the curve's grip at 26 C");
        RequireNear(cold.PressureFactor(), 1.0 + (33.0 / 28.0 - 1.0) * 0.55, 1e-12, "and the pressure's rolling factor");
    }
    const double factor = 1.0 + (33.0 / 28.0 - 1.0) * 0.55;
    // The core towards the rolling heat ROLLING_K * factor * spin * load / 1000.
    {
        TyreThermalParameters p = Rx7Semislicks();
        p.surfaceTransfer = p.patchTransfer = p.coreTransfer = 0.0;
        TyreThermalModel model(p);
        TyreThermalInput in;
        in.dt = dt;
        in.wheelSpeed = spin;
        in.load = load;
        model.Step(in);
        const double target = 0.18 * factor * spin * load * 0.001;
        RequireNear(model.CoreTemperature(), 26.0 + (target - 26.0) * dt * 0.0029, 1e-12, "the core's step towards its rolling heat");
        RequireNear(model.Pressure(), 28.0 + (model.CoreTemperature() - 26.0) * 0.16, 1e-12, "the pressure 0.16 psi per degree of core");
    }
    // The patch on the road towards its heat level: sliding (speed * load * grip * FRICTION_K * road) and rolling
    // (SURFACE_ROLLING_K * factor * spin * load / 1000) over the road's 26 C, by lane.
    {
        TyreThermalParameters p = Rx7Semislicks();
        p.patchTransfer = p.coreTransfer = p.internalCoreTransfer = 0.0;
        TyreThermalModel model(p);
        TyreThermalInput in;
        in.dt = dt;
        in.wheelSpeed = spin;
        in.load = load;
        in.slideSpeed = 2.0;
        in.grip = 1.3;
        in.camber = 0.05;
        model.Step(in);
        const double heat = 2.0 * load * 1.3 * 0.06446 + factor * 0.96443 * spin * load * 0.001;
        const double ratio = 28.0 / 33.0 - 1.0;
        const double spread = std::clamp(0.05 * 1.4, -1.0, 1.0);
        const double level = (heat + 26.0) * (1.0 - 0.5 * ratio);
        const std::array<double, 3> expected{
            26.0 + ((1.0 + spread - 0.05 * ratio) * level - 26.0) * dt * 0.015,
            26.0 + ((1.0 + 0.1 * ratio) * level - 26.0) * dt * 0.015,
            26.0 + ((1.0 - spread - 0.05 * ratio) * level - 26.0) * dt * 0.015};
        for (int lane = 0; lane < 3; ++lane)
        {
            RequireNear(model.Patch(lane, 0), expected[static_cast<size_t>(lane)], 1e-9, "the contact patch's lane " + std::to_string(lane));
            RequireNear(model.Patch(lane, 5), 26.0, 0.0, "a patch off the road stays at the air's temperature");
        }
        Require(model.Patch(0, 0) > model.Patch(2, 0), "camber to the right heats lane 0 more");
    }
    // Off the road a patch cools towards the air at SURFACE_TRANSFER times 1 + speed^2 (COOL_FACTOR - 1) 0.000324.
    {
        TyreThermalParameters p = Rx7Semislicks();
        p.patchTransfer = p.coreTransfer = p.internalCoreTransfer = 0.0;
        TyreThermalModel model(p, 80.0);
        TyreThermalInput in;
        in.dt = dt;
        in.carSpeed = 30.0;
        model.Step(in);
        RequireNear(model.Patch(1, 3), 80.0 + (26.0 - 80.0) * (1.0 + 900.0 * 1.17 * 0.000324) * 0.015 * dt, 1e-12, "the cooling, faster with speed");
    }
    // A patch shares with its four neighbours and the core.
    {
        TyreThermalParameters p = Rx7Semislicks();
        p.surfaceTransfer = p.internalCoreTransfer = 0.0;
        p.patchTransfer = 0.1;
        p.coreTransfer = 0.0;
        TyreThermalModel model(p, 26.0);
        TyreThermalInput in;
        in.dt = 0.01;
        in.wheelSpeed = 0.0;
        in.load = load;
        in.slideSpeed = 5.0;
        in.grip = 1.3;
        // Surface transfer off: the contact patch keeps 26 C; nothing moves.
        model.Step(in);
        RequireNear(model.Patch(1, 0), 26.0, 1e-12, "no transfer, no heat");
    }
    // The contact's temperature: the lanes weighted 1 + camber spread, 1, 1 - spread; the practical one a quarter
    // of the way from the core to it, and the grip the curve's there.
    {
        TyreThermalParameters p = Rx7Semislicks();
        TyreThermalModel model(p, 26.0);
        TyreThermalInput in;
        in.dt = 0.002;
        in.wheelSpeed = 0.0;
        in.load = load;
        in.slideSpeed = 3.0;
        in.grip = 1.3;
        in.camber = -0.2;
        for (int step = 0; step < 2000; ++step)
        {
            model.Step(in);
        }
        const std::array<double, 3> t = model.ContactTemperatures();
        const double spread = std::clamp(-0.2 * 1.4, -1.0, 1.0);
        RequireNear(model.ContactTemperature(-0.2), ((1.0 + spread) * t[0] + t[1] + (1.0 - spread) * t[2]) / 3.0, 1e-12, "the contact's mix of lanes");
        Require(t[2] > t[0], "camber to the left heats lane 2 more");
        const double practical = model.CoreTemperature() + (model.ContactTemperature(-0.2) - model.CoreTemperature()) * 0.25;
        RequireNear(model.PracticalTemperature(), practical, 1e-12, "the practical temperature");
        RequireNear(model.Performance(), EvaluateThermalCurve(p.performanceCurve, practical), 1e-12, "and the grip from the curve");
        Require(model.ContactTemperature(-0.2) > 40.0, "four seconds of sliding in place heat the patch: " + std::to_string(model.ContactTemperature(-0.2)));
    }
}

// Driving: a minute at 100 km/h rolling warms the tread and the core and raises the pressure; turning round, the
// patches take turns on the road and warm alike; parked, the tyre cools back to the air.
void TestThermalModelWarmsRollingAndCools()
{
    TyreThermalModel model(Rx7Semislicks());
    TyreThermalInput in;
    in.dt = 0.001;
    in.load = 3000.0;
    in.carSpeed = 100.0 / 3.6;
    in.wheelSpeed = in.carSpeed / 0.312;
    in.grip = 1.3;
    for (int step = 0; step < 60000; ++step)
    {
        model.Step(in);
    }
    const std::array<double, 3> lanes = model.LaneTemperatures();
    std::cout << "  thermal: a minute at 100 km/h: tread " << lanes[0] << " / " << lanes[1] << " / " << lanes[2] << " C, core " << model.CoreTemperature()
              << " C, " << model.Pressure() << " psi, grip " << model.Performance() << '\n';
    Require(lanes[1] > 26.5 && model.CoreTemperature() > 26.0 && model.Pressure() > 28.0, "rolling warms the tyre and raises its pressure");
    double lowest = 1e9;
    double highest = -1e9;
    for (int index = 0; index < kThermalPatches; ++index)
    {
        lowest = std::min(lowest, model.Patch(1, index));
        highest = std::max(highest, model.Patch(1, index));
    }
    Require(highest - lowest < 0.5 * (lanes[1] - 26.0) + 0.5, "the patches round the tyre warm alike as it turns");
    // Sliding warms faster.
    TyreThermalModel sliding(Rx7Semislicks());
    in.slideSpeed = 1.5;
    for (int step = 0; step < 60000; ++step)
    {
        sliding.Step(in);
    }
    Require(sliding.LaneTemperatures()[1] > lanes[1] + 5.0, "sliding heats the tread more than rolling alone");
    // Parked.
    in = TyreThermalInput{};
    in.dt = 0.01;
    for (int step = 0; step < 100000; ++step)
    {
        sliding.Step(in);
    }
    RequireNear(sliding.LaneTemperatures()[1], 26.0, 1.0, "parked, the tread cools to the air");
}

// The game's wear: virtual km from the distance slid (times the load over FZ0 with USE_LOAD), the grip WEAR_CURVE
// leaves by them, graining below the performance curve's window and blistering above it, each step against its
// formula, with the RX-7 Tuned's semislicks.
void TestWearModelFollowsTheGame()
{
    TyreWearParameters p;
    p.wearCurve = {{0.0f, 100.0f}, {0.25f, 100.0f}, {10.0f, 98.0f}, {25.0f, 80.0f}, {27.5f, 70.0f}};
    p.useLoad = true;
    p.referenceLoad = 2860.0;
    p.grainGain = 0.4;
    p.grainGamma = 1.0;
    p.blisterGain = 0.3;
    p.blisterGamma = 1.0;
    p.performanceCurve = Rx7Semislicks().performanceCurve;
    TyreWearModel model(p);
    RequireNear(model.GrainBelow(), 75.0, 0.0, "graining below the window's start, where the curve first reaches 1");
    RequireNear(model.BlisterAbove(), 95.0, 0.0, "blistering above its end, the last 1");
    RequireNear(model.Grip(), 1.0, 0.0, "new tyres grip fully");

    // Sliding 2 m/s for 10 s at twice FZ0: 0.04 virtual km; cold or hot nothing else without temperatures.
    TyreWearInput in;
    in.dt = 10.0;
    in.slideSpeed = 2.0;
    in.contactSpeed = 20.0;
    in.load = 2.0 * 2860.0;
    in.slip = 1.0;
    model.Step(in);
    RequireNear(model.VirtualKm(), 2.0 * 10.0 * 2.0 * 0.001, 1e-12, "virtual km, the load counted");
    Require(model.Grain() == 0.0 && model.Blister() == 0.0, "no graining or blistering without temperatures");

    // A second of graining at 50 C (25 below the window): 20 m/s * 0.4 * 25 * 0.0001, less 20 * 0.4 * 0.00005 worn off.
    model.Reset();
    in.dt = 1.0;
    in.slideSpeed = 0.0;
    in.temperatures = true;
    in.coreTemperature = 50.0;
    model.Step(in);
    RequireNear(model.Grain(), 20.0 * 0.4 * 25.0 * 0.0001 - 20.0 * 0.4 * 0.00005, 1e-12, "graining below the window");
    // And blistering at 105 C (10 above it): 20 * 0.3 * 10 * 0.0001.
    model.Reset();
    in.coreTemperature = 105.0;
    model.Step(in);
    RequireNear(model.Blister(), 20.0 * 0.3 * 10.0 * 0.0001, 1e-12, "blistering above the window");
    // Slow, or on a slippery road, neither.
    model.Reset();
    in.contactSpeed = 1.5;
    model.Step(in);
    in.contactSpeed = 20.0;
    in.surfaceGrip = 0.9;
    model.Step(in);
    Require(model.Blister() == 0.0, "nothing blisters slowly or on a slippery road");

    // The grip: the wear curve (%, scaled) over 1 + 0.2 of the blister share.
    TyreWearModel worn(p);
    TyreWearInput slide;
    slide.dt = 1000.0;
    slide.slideSpeed = 25.0;
    slide.load = 2860.0;
    worn.Step(slide);
    RequireNear(worn.VirtualKm(), 25.0, 1e-9, "25 km slid");
    RequireNear(worn.Grip(), 0.8, 1e-9, "the wear curve's 80 % at 25 km");
    TyreWearInput blistering;
    blistering.dt = 1e6;
    blistering.contactSpeed = 20.0;
    blistering.load = 2860.0;
    blistering.slip = 2.5;
    blistering.temperatures = true;
    blistering.coreTemperature = 140.0;
    worn.Step(blistering);
    RequireNear(worn.Blister(), 100.0, 0.0, "blistering stops at 100");
    RequireNear(worn.Grip(), 0.8 / 1.2, 1e-9, "and takes a fifth more");
}

int main()
{
    const struct
    {
        const char* name;
        void (*run)();
    } tests[] = {
        {"TestThermalModelStepsAsTheGame", TestThermalModelStepsAsTheGame},
        {"TestThermalModelWarmsRollingAndCools", TestThermalModelWarmsRollingAndCools},
        {"TestWearModelFollowsTheGame", TestWearModelFollowsTheGame},
        {"TestRigidBrushMatchesTheClosedForm", TestRigidBrushMatchesTheClosedForm},
        {"TestBrushStaysInsideTheFrictionEllipse", TestBrushStaysInsideTheFrictionEllipse},
        {"TestFittedBrushPeaksWhereAskedAndPointsTheRightWay", TestFittedBrushPeaksWhereAskedAndPointsTheRightWay},
        {"TestBrushPatchFollowsTheInflationPressure", TestBrushPatchFollowsTheInflationPressure},
        {"TestBrushRelaxesOverTheGivenLength", TestBrushRelaxesOverTheGivenLength},
        {"TestBrushForceBuildsOverItsRelaxationLength", TestBrushForceBuildsOverItsRelaxationLength},
        {"TestBrushStandsAndHolds", TestBrushStandsAndHolds},
        {"TestBrushSquashesNoFurtherThanTheRim", TestBrushSquashesNoFurtherThanTheRim},
        {"TestBrushGripsEachWayItsOwn", TestBrushGripsEachWayItsOwn},
        {"TestBrushPeaksAtTheSlipRatioAskedFor", TestBrushPeaksAtTheSlipRatioAskedFor},
        {"TestBrushWithoutFalloffReachesItsLimitWhereAsked", TestBrushWithoutFalloffReachesItsLimitWhereAsked},
        {"TestFinelyCutBrushAgreesWithTheDefault", TestFinelyCutBrushAgreesWithTheDefault},
    };
    int failures = 0;
    for (const auto& test : tests)
    {
        try
        {
            test.run();
            std::cout << "[pass] " << test.name << "\n";
        }
        catch (const std::exception& e)
        {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << e.what() << "\n";
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
