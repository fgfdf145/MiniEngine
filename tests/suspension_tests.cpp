#include <engine/suspension/suspension_axle.h>
#include <engine/suspension/suspension_corner.h>
#include <engine/suspension/suspension_friction.h>
#include <engine/suspension/suspension_kinematics.h>
#include <engine/suspension/suspension_model.h>
#include <engine/suspension/suspension_rigs.h>
#include <engine/suspension/suspension_statics.h>
#include <engine/suspension/suspension_strut.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me::suspension;

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

// ---- Reference suspensions ----

// Rill, Road Vehicle Dynamics (2012), Listing 5.6: a typical passenger car's front left double
// wishbone, origin at the axle centre. Rill gives no spring; the seat here is arbitrary.
SuspensionDefinition RillDoubleWishbone()
{
    DoubleWishboneHardpoints hp;
    hp.wheelCenter = {0.0000, 0.7680, 0.0000};
    hp.lowerRear = {-0.2510, 0.3200, -0.0800};
    hp.lowerFront = {0.1480, 0.3200, -0.0940};
    hp.lowerBall = {0.0130, 0.7370, -0.1450};
    hp.upperRear = {-0.1050, 0.4350, 0.1960};
    hp.upperFront = {0.1220, 0.4350, 0.2300};
    hp.upperBall = {-0.0250, 0.6800, 0.1620};
    hp.tieInner = {-0.1500, 0.3800, -0.0380};
    hp.tieOuter = {-0.1370, 0.6900, -0.0880};
    hp.springLower = {0.0000, 0.6000, -0.1150};
    hp.springUpper = {0.0000, 0.5500, 0.3500};
    // Rill's wheel axis [toe0; 1; -camb0] with camb0 = 0.8 deg written in radians.
    return MakeDoubleWishbone(hp, 0.285, glm::normalize(Vec3(0.0, 1.0, -0.8 * kDeg)));
}

// Assetto Corsa's coordinates (x towards the car's centre, y up, z forward, wheel centre origin)
// for a left wheel, turned into x forward, y outward/left, z up.
Vec3 FromAc(double x, double y, double z)
{
    return {z, -x, y};
}

// ks_porsche_718_boxster_s, suspensions.ini [FRONT] / [REAR], TYPE=STRUT.
SuspensionDefinition BoxsterStrut(bool front)
{
    MacPhersonHardpoints hp;
    hp.wheelCenter = {0.0, 0.0, 0.0};
    if (front)
    {
        hp.strutTop = FromAc(0.28497, 0.40218, -0.08294);
        hp.strutLower = FromAc(0.10784, -0.16402, 0.01798);
        hp.lowerFront = FromAc(0.43800, -0.16775, 0.26073);
        hp.lowerRear = FromAc(0.41057, -0.15672, -0.01280);
        hp.lowerBall = FromAc(0.10784, -0.16402, 0.01798);
        hp.tieInner = FromAc(0.48843, -0.09289, 0.10865);
        hp.tieOuter = FromAc(0.09707, -0.08479, 0.14781);
    }
    else
    {
        hp.strutTop = FromAc(0.3355, 0.4601, -0.0506);
        hp.strutLower = FromAc(0.1106, -0.1804, 0.0095);
        hp.lowerFront = FromAc(0.2688, -0.0641, 0.6175);
        hp.lowerRear = FromAc(0.4054, -0.1744, -0.0565);
        hp.lowerBall = FromAc(0.1106, -0.1804, 0.0095);
        hp.tieInner = FromAc(0.5410, -0.1257, 0.1950);
        hp.tieOuter = FromAc(0.2082, -0.1351, 0.2028);
        hp.steered = false;
    }
    SuspensionDefinition def = MakeMacPherson(hp, 0.335, Vec3(0.0, 1.0, 0.0));
    def.vehicleCenter = Vec3(0.0, front ? -1.515 / 2.0 : -1.540 / 2.0, 0.0);
    return def;
}

// A MacPherson whose strut axis does not pass through the lower ball joint (the general case).
SuspensionDefinition OffsetStrut()
{
    SuspensionDefinition def = BoxsterStrut(true);
    MacPhersonHardpoints hp;
    const Model m = Compile(def);
    hp.wheelCenter = m.design[m.Find("wheel_center")];
    hp.strutTop = m.design[m.Find("strut_top")];
    hp.lowerFront = m.design[m.Find("lower_front")];
    hp.lowerRear = m.design[m.Find("lower_rear")];
    hp.lowerBall = m.design[m.Find("lower_ball")];
    hp.tieInner = m.design[m.Find("tie_inner")];
    hp.tieOuter = m.design[m.Find("tie_outer")];
    // 40% of the way up the strut and 30 mm outboard of the old axis: the axis moves with it.
    hp.strutLower = hp.lowerBall + 0.4 * (hp.strutTop - hp.lowerBall) + Vec3(0.0, 0.03, 0.0);
    return MakeMacPherson(hp, 0.335, Vec3(0.0, 1.0, 0.0));
}

struct ReferencePoint
{
    double travel;
    double rack;
    double wheelX;
    double camberDeg;
    double toeDeg;
    double strutLength; // MacPherson only
};

// From tools/suspension_reference/reference_curves.py (closed-form solutions checked against Rill's
// printed results and an independent Newton solve).
const ReferencePoint kRillPoints[] = {
    {-0.070198595737, 0, 0.003566817762, -0.9989079132, -0.2385631219, 0},
    {-0.035554980921, 0, 0.001789560499, 0.2123355159, -0.1014182747, 0},
    {0.036150990285, 0, -0.001787906359, 0.9290553981, 0.0841129767, 0},
    {0.072591766230, 0, -0.003557648744, 0.6719665150, 0.1595177544, 0},
    {-0.002469436013, -0.04, -0.017970123956, 3.4923888587, -17.7961230844, 0},
    {0.001292042557, 0.04, 0.016007488474, -0.7571854944, 16.2135376094, 0},
    {0.037460189977, 0.03, 0.010106688461, -0.5676288193, 12.0156654850, 0},
};
const ReferencePoint kBoxsterFrontPoints[] = {
    {-0.083947953521, 0, -0.001580486785, 3.0035088703, -0.5134602853, 0.672458811656},
    {-0.044806970479, 0, -0.000164128213, 1.4541275710, -0.1465787783, 0.640329253364},
    {0.044027040146, 0, -0.001170776994, -1.0486492517, -0.0417306382, 0.561696781152},
    {0.081173285397, 0, -0.003113596782, -1.5618498736, -0.1518295130, 0.525762308916},
    {0.004011889692, -0.03, 0.032779209980, -1.7218188384, 11.9893661825, 0.601782621301},
    {-0.006681019820, 0.03, -0.034852605154, 2.6656196257, -12.5532122286, 0.601782621301},
    {0.039664666962, 0.02, -0.023516287988, 0.7026758172, -8.0661448566, 0.561696781152},
};
const ReferencePoint kBoxsterRearPoints[] = {
    {-0.075581543602, 0, 0.009505445168, 2.8738004907, -0.7944780347, 0.744651235284},
    {-0.040505910499, 0, 0.005347293272, 1.4084788119, -0.2335147944, 0.716114617085},
    {0.040259236133, 0, -0.005647332915, -1.0922441999, -0.1187878485, 0.645096881473},
    {0.074678010413, 0, -0.010551194921, -1.7488275914, -0.4346772724, 0.612150947068},
};

template <std::size_t N>
void CheckCurve(const SuspensionDefinition& def, const ReferencePoint (&points)[N], const std::string& what)
{
    for (const ReferencePoint& p : points)
    {
        Kinematics kin(Compile(def));
        // Walk there in small steps as a running simulation would.
        const int steps = 50;
        for (int i = 1; i <= steps; ++i)
        {
            const SolveReport& r = kin.Solve(p.travel * i / steps, p.rack * i / steps);
            Require(r.status == SolveStatus::Converged, what + ": solve failed on the way");
        }
        KinematicOutputs out;
        ComputeOutputs(kin, out);
        const std::string at = what + " at travel " + std::to_string(p.travel) + ", rack " + std::to_string(p.rack);
        RequireNear(out.wheelCenter.x, p.wheelX, 1e-8, at + ": wheel centre x");
        RequireNear(out.camber / kDeg, p.camberDeg, 1e-6, at + ": camber");
        RequireNear(out.toe / kDeg, p.toeDeg, 1e-6, at + ": toe");
        if (p.strutLength > 0.0)
        {
            RequireNear(out.elements[0].length, p.strutLength, 1e-8, at + ": strut length");
        }
    }
}

// ---- Tests ----

void TestModelsAreDeterminate()
{
    const Model dw = Compile(RillDoubleWishbone());
    Require(dw.unknowns == dw.rows && dw.unknowns == 18, "double wishbone: 6 moving points = 18 unknowns, 18 equations");
    const Model mp = Compile(BoxsterStrut(true));
    Require(mp.unknowns == mp.rows && mp.unknowns == 16, "MacPherson with the strut through the ball joint: 5 points + slide = 16");
    const Model offset = Compile(OffsetStrut());
    Require(offset.unknowns == offset.rows && offset.unknowns == 19, "MacPherson with an offset strut: 6 points + slide = 19");

    // Leaving out the tie rod makes the knuckle free to steer: refused.
    SuspensionDefinition loose = RillDoubleWishbone();
    loose.bodies.pop_back();
    bool threw = false;
    try
    {
        Compile(loose);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Require(threw, "a mechanism with a free degree of freedom must be refused");
}

void TestRillSteeringGeometryMatchesTheBook()
{
    Kinematics kin(Compile(RillDoubleWishbone()));
    KinematicOutputs out;
    ComputeOutputs(kin, out);
    Require(out.kingpinValid, "the steering axis exists");
    // Rill 2012, p. 155: kingpin 10.5182 deg, caster 7.0561 deg, caster offset 0.030326 m, scrub 0.0010327 m.
    RequireNear(out.kingpinInclination / kDeg, 10.5182, 1e-4, "kingpin inclination");
    RequireNear(out.caster / kDeg, 7.0561, 1e-4, "caster");
    RequireNear(out.casterTrail, 0.030326, 1e-6, "caster offset");
    RequireNear(out.scrubRadius, 0.0010327, 1e-7, "scrub radius");
    RequireNear(out.camber / kDeg, 0.8, 1e-3, "static camber");
}

void TestRollCenterAgreesWithTheContactPath()
{
    // Symmetric axle, small motions: h = (track / 2) * d(half track)/dz (Milliken's construction in
    // one formula), measured from the road.
    for (const SuspensionDefinition& def : {RillDoubleWishbone(), BoxsterStrut(true), BoxsterStrut(false)})
    {
        Kinematics kin(Compile(def));
        KinematicOutputs at;
        KinematicOutputs up;
        KinematicOutputs down;
        ComputeOutputs(kin, at);
        const double h = 1e-4;
        kin.Solve(h, 0.0);
        ComputeOutputs(kin, up);
        kin.Solve(-h, 0.0);
        ComputeOutputs(kin, down);
        const double slope = (up.halfTrackChange - down.halfTrackChange) / (2.0 * h);
        const double halfTrack = std::abs(at.contactPoint.y - def.vehicleCenter.y);
        RequireNear(at.rollCenterHeight, halfTrack * slope, 2e-3, def.name + ": roll centre height");
    }
}

void TestCurvesMatchClosedFormSolutions()
{
    CheckCurve(RillDoubleWishbone(), kRillPoints, "Rill double wishbone");
    CheckCurve(BoxsterStrut(true), kBoxsterFrontPoints, "Boxster front strut");
    CheckCurve(BoxsterStrut(false), kBoxsterRearPoints, "Boxster rear strut");
}

void TestMirroredCornerReportsTheSame()
{
    for (const SuspensionDefinition& left : {RillDoubleWishbone(), BoxsterStrut(true)})
    {
        Kinematics l(Compile(left));
        Kinematics r(Compile(MirrorToRight(left)));
        for (int i = 1; i <= 40; ++i)
        {
            // The same rack travel steers the right wheel the other way.
            l.Solve(0.05 * i / 40.0, 0.02 * i / 40.0);
            r.Solve(0.05 * i / 40.0, -0.02 * i / 40.0);
        }
        KinematicOutputs a;
        KinematicOutputs b;
        ComputeOutputs(l, a);
        ComputeOutputs(r, b);
        RequireNear(b.camber, a.camber, 1e-10, left.name + ": mirrored camber");
        RequireNear(b.toe, a.toe, 1e-10, left.name + ": mirrored toe");
        RequireNear(b.kingpinInclination, a.kingpinInclination, 1e-9, left.name + ": mirrored kingpin");
        RequireNear(b.scrubRadius, a.scrubRadius, 1e-9, left.name + ": mirrored scrub");
        RequireNear(b.halfTrackChange, a.halfTrackChange, 1e-10, left.name + ": mirrored track change");
        RequireNear(b.camberPerTravel, a.camberPerTravel, 1e-8, left.name + ": mirrored camber gain");
    }
}

void TestNewtonNeedsOneOrTwoCorrections()
{
    // A hard 1 kHz workout: +/-80 mm at 3 Hz (peak 1.5 m/s) and the rack +/-30 mm at 1 Hz.
    for (const SuspensionDefinition& def : {RillDoubleWishbone(), BoxsterStrut(true), OffsetStrut()})
    {
        Kinematics kin(Compile(def));
        int frames = 0;
        int corrections = 0;
        int worst = 0;
        int factorisations = 0;
        double worstResidual = 0.0;
        for (int i = 1; i <= 3000; ++i)
        {
            const double t = i * 1e-3;
            const SolveReport& r = kin.Solve(0.08 * std::sin(2.0 * kPi * 3.0 * t), 0.03 * std::sin(2.0 * kPi * t));
            Require(r.status == SolveStatus::Converged, def.name + ": every frame converges");
            ++frames;
            corrections += r.iterations;
            factorisations += r.factorisations;
            worst = std::max(worst, r.iterations);
            worstResidual = std::max(worstResidual, r.residual);
        }
        const double average = static_cast<double>(corrections) / frames;
        std::cout << "  " << def.name << ": " << average << " corrections/frame (worst " << worst << "), "
                  << static_cast<double>(factorisations) / frames << " LU/frame, worst residual " << worstResidual << "\n";
        Require(average <= 2.0, def.name + ": one or two corrections a frame on average");
        Require(worst <= 3, def.name + ": never more than three corrections");
        Require(worstResidual < 1e-10, def.name + ": residual within tolerance");
    }
}

void TestSensitivitiesAndAccelerations()
{
    for (const SuspensionDefinition& def : {RillDoubleWishbone(), OffsetStrut()})
    {
        const Model model = Compile(def);
        // Finite differences need positions far tighter than the frame-to-frame tolerance.
        SolverSettings tight;
        tight.residualTolerance = 1e-15;
        Kinematics kin(model, tight);
        kin.Solve(0.03, 0.01);
        const std::vector<double> q0 = kin.Coordinates();
        const std::vector<double> dz = kin.DqDTravel();
        const std::vector<double> du = kin.DqDRack();

        const double h = 1e-6;
        Kinematics plusZ(model, tight);
        plusZ.Solve(0.03, 0.01);
        plusZ.Solve(0.03 + h, 0.01);
        Kinematics minusZ(model, tight);
        minusZ.Solve(0.03, 0.01);
        minusZ.Solve(0.03 - h, 0.01);
        Kinematics plusU(model, tight);
        plusU.Solve(0.03, 0.01);
        plusU.Solve(0.03, 0.01 + h);
        Kinematics minusU(model, tight);
        minusU.Solve(0.03, 0.01);
        minusU.Solve(0.03, 0.01 - h);
        for (std::size_t i = 0; i < q0.size(); ++i)
        {
            RequireNear(dz[i], (plusZ.Coordinates()[i] - minusZ.Coordinates()[i]) / (2 * h), 1e-6, def.name + ": dq/dz");
            RequireNear(du[i], (plusU.Coordinates()[i] - minusU.Coordinates()[i]) / (2 * h), 1e-6, def.name + ": dq/du");
        }

        // Along z(t) = 0.03 + 0.2 t + 0.5 t^2 and u(t) = 0.01 - 0.05 t + 0.3 t^2, at t = 0.
        const auto path = [](double t, double& z, double& u) {
            z = 0.03 + 0.2 * t + 0.5 * t * t;
            u = 0.01 - 0.05 * t + 0.3 * t * t;
        };
        std::vector<double> qdd;
        kin.Accelerations(0.2, -0.05, 1.0, 0.6, qdd);
        const double dt = 1e-3;
        std::vector<std::vector<double>> samples;
        for (double t : {-dt, dt})
        {
            Kinematics k(model, tight);
            k.Solve(0.03, 0.01);
            double z = 0.0;
            double u = 0.0;
            path(t, z, u);
            k.Solve(z, u);
            samples.push_back(k.Coordinates());
        }
        for (std::size_t i = 0; i < q0.size(); ++i)
        {
            const double second = (samples[1][i] - 2.0 * q0[i] + samples[0][i]) / (dt * dt);
            RequireNear(qdd[i], second, 1e-4 * (1.0 + std::abs(second)), def.name + ": qdd");
        }
    }
}

void TestOutOfReachIsClampedAndRecovers()
{
    Kinematics kin(Compile(BoxsterStrut(true)));
    // 600 mm of bump is far beyond what the lower arm and strut can reach.
    const SolveReport report = kin.Solve(0.6, 0.0);
    Require(report.status == SolveStatus::Clamped, "an unreachable travel is clamped");
    Require(report.travel > 0.05 && report.travel < 0.6, "it got part of the way");
    Require(report.residual < 1e-10, "the clamped position is still a solution");
    KinematicOutputs out;
    ComputeOutputs(kin, out);
    Require(std::isfinite(out.camber) && std::isfinite(out.toe), "outputs stay finite at the limit");
    const SolveReport back = kin.Solve(0.0, 0.0);
    Require(back.status == SolveStatus::Converged, "it comes back from the limit");
    ComputeOutputs(kin, out);
    RequireNear(glm::length(out.wheelCenterChange), 0.0, 1e-9, "and lands on the design position");
}

void TestReactionsBalanceTheLoad()
{
    for (const SuspensionDefinition& def : {RillDoubleWishbone(), BoxsterStrut(true), OffsetStrut()})
    {
        const Model model = Compile(def);
        Kinematics kin(model);
        kin.Solve(0.02, 0.01);
        WheelLoad load;
        load.force = {-3000.0, 1500.0, 4000.0}; // braking, cornering, wheel load
        load.moment = {0.0, 0.0, 40.0};         // aligning moment
        const std::vector<double> elementForces(model.elementA.size(), 2500.0);
        Reactions reactions;
        SolveReactions(kin, load, elementForces, reactions);

        // A massless mechanism passes everything to the chassis: forces and moments balance.
        KinematicOutputs out;
        ComputeOutputs(kin, out);
        Vec3 force = load.force;
        Vec3 moment = load.moment + glm::cross(out.contactPoint, load.force);
        // What the mechanism puts on the chassis and rack, summed over the points that are not unknowns.
        Vec3 chassisForce(0.0);
        Vec3 chassisMoment(0.0);
        for (std::size_t i = 0; i < model.pointNames.size(); ++i)
        {
            if (model.slot[i] >= 0)
            {
                continue;
            }
            const Vec3 p = kin.Point(static_cast<int>(i));
            chassisForce += reactions.pointForces[i];
            chassisMoment += glm::cross(p, reactions.pointForces[i]);
        }
        // The travel driver holds the wheel too: its reaction acts at the wheel centre.
        const Vec3 driver = -reactions.multipliers[model.rows - 1] * def.travelAxis;
        const Vec3 w = kin.Point(model.wheelCenter);
        RequireNear(glm::length(force - chassisForce + driver), 0.0, 1e-6, def.name + ": forces balance");
        RequireNear(glm::length(moment - chassisMoment + glm::cross(w, driver)), 0.0, 1e-6, def.name + ": moments balance");

        // The travel force is the load's virtual work per metre of travel (the element pushes too).
        const Vec3 omega = KnuckleAngularRate(kin, kin.DqDTravel(), 0.0);
        const Vec3 vp = kin.PointDTravel(model.wheelCenter) + glm::cross(omega, out.contactPoint - w);
        const double expected = glm::dot(load.force, vp) + glm::dot(load.moment, omega) + elementForces[0] * out.elements[0].lengthPerTravel;
        RequireNear(reactions.travelForce, expected, 1e-6, def.name + ": travel force is the virtual work");
    }
}

void TestStrutSideLoadFollowsBraking()
{
    Kinematics kin(Compile(BoxsterStrut(true)));
    Reactions reactions;
    WheelLoad load;
    load.force = {0.0, 0.0, 4000.0};
    SolveReactions(kin, load, {3800.0}, reactions);
    const double straight = reactions.sliderSideLoad[0];
    load.force.x = -4000.0; // braking
    SolveReactions(kin, load, {3800.0}, reactions);
    const double braking = reactions.sliderSideLoad[0];
    std::cout << "  strut side load: " << straight << " N straight, " << braking << " N braking 4 kN\n";
    Require(straight > 50.0, "a strut carries side load from the wheel load's offset alone");
    Require(braking > straight + 200.0, "braking loads the strut sideways");
}

// The Rill double wishbone with rubber at its four arm pivots.
SuspensionDefinition BushedDoubleWishbone(const Curve& radial, const Curve& axial)
{
    SuspensionDefinition def = RillDoubleWishbone();
    for (const char* name : {"lower_front", "lower_rear", "upper_front", "upper_rear"})
    {
        BushingDef b;
        b.point = name;
        // Local x along the vehicle (the pivot axis direction, axial), y and z radial.
        b.x = axial;
        b.y = radial;
        b.z = radial;
        def.bushings.push_back(b);
    }
    return def;
}

double ToeUnderSideForce(const SuspensionDefinition& def, double force)
{
    Compliance compliance(Compile(def, ModelMode::Compliant));
    WheelLoad load;
    load.force = {0.0, force, 0.0};
    Compliance::Report report;
    const int steps = 4;
    for (int i = 1; i <= steps; ++i)
    {
        load.force.y = force * i / steps;
        report = compliance.Solve(0.0, 0.0, load, {0.0});
    }
    Require(report.converged, "compliance converges");
    Require(report.residual < 1e-6, "compliance residual small");
    return compliance.Attitude().toe;
}

void TestComplianceIsNonlinearWhereTheBushingsAre()
{
    const double toe0 = ToeUnderSideForce(BushedDoubleWishbone(Curve::Linear(1e10), Curve::Linear(1e10)), 0.0);
    // Nearly rigid bushings: the toe barely moves.
    const double stiff = ToeUnderSideForce(BushedDoubleWishbone(Curve::Linear(1e10), Curve::Linear(1e10)), 3000.0) - toe0;
    std::cout << "  toe change with near-rigid bushings: " << stiff << " rad\n";
    Require(std::abs(stiff) < 1e-5, "rigid bushings, no compliance steer");

    // Linear bushings: twice the force, twice the toe change (small deflections).
    const SuspensionDefinition linear = BushedDoubleWishbone(Curve::Linear(4e6), Curve::Linear(1e6));
    const double one = ToeUnderSideForce(linear, 1000.0) - toe0;
    const double two = ToeUnderSideForce(linear, 2000.0) - toe0;
    std::cout << "  lateral force compliance steer: " << one / kDeg << " deg/kN (linear bushings)\n";
    Require(std::abs(one) > 1e-5, "side force steers the wheel through the bushings");
    RequireNear(two / one, 2.0, 0.02, "linear bushings: toe change doubles with the force");

    // Progressive bushings stiffen: twice the force gives clearly less than twice the toe change.
    const SuspensionDefinition progressive = BushedDoubleWishbone(Curve::Progressive(4e6, 4e7, 0.001), Curve::Progressive(1e6, 1e7, 0.002));
    const double pOne = ToeUnderSideForce(progressive, 1000.0) - toe0;
    const double pFour = ToeUnderSideForce(progressive, 4000.0) - toe0;
    Require(std::abs(pFour) < 3.6 * std::abs(pOne), "progressive bushings: less than proportional");
}

void TestLuGreSteadyStateIsTheStribeckCurve()
{
    LuGreParameters p;
    p.stribeck.coulomb = {100.0, 0.05, 0.0};
    p.stribeck.breakaway = {160.0, 0.08, 0.0};
    p.stribeck.stribeckVelocity = 0.005;
    p.stribeck.viscous = 50.0;
    for (double normal : {0.0, 2000.0})
    {
        for (double v : {0.001, 0.004, 0.02, 0.2})
        {
            LuGreFriction friction(p);
            double force = 0.0;
            // Forty bristle time constants g / (sigma0 |v|).
            const int steps = static_cast<int>(40.0 * StribeckCurve(p.stribeck, v, normal) / (p.bristleStiffness * v) / 1e-3) + 100;
            for (int i = 0; i < steps; ++i)
            {
                force = friction.Evaluate(v, normal, 1e-3);
                friction.Commit(v, normal, 1e-3);
            }
            RequireNear(force, StribeckCurve(p.stribeck, v, normal) + p.stribeck.viscous * v, 1e-6, "LuGre steady state");
        }
    }
    // Side load raises the breakaway force: 160 + 0.08 * 2000 = 320 N.
    RequireNear(StribeckCurve(p.stribeck, 0.0, 2000.0), 320.0, 1e-9, "breakaway with side load");
}

void TestLuGreSticksBelowBreakaway()
{
    // A spring pulled slowly through a LuGre contact: the force rises to breakaway before sliding.
    LuGreParameters p;
    p.stribeck.coulomb = {100.0, 0.0, 0.0};
    p.stribeck.breakaway = {150.0, 0.0, 0.0};
    p.stribeck.stribeckVelocity = 0.002;
    p.bristleStiffness = 1e6;
    p.bristleDamping = 2000.0;
    LuGreFriction friction(p);
    // Below breakaway with no slip velocity left the contact holds: apply v = 0 and see it keep force.
    for (int i = 0; i < 100; ++i)
    {
        friction.Commit(1e-3, 0.0, 1e-3); // 0.1 mm of travel, mostly taken up by the bristles
    }
    // At rest the bristles keep their deflection, so the contact keeps its force for as long as it
    // stays at rest: stiction, which the static Stribeck model cannot do.
    const double loaded = 1e6 * friction.BristleDeflection();
    Require(loaded > 50.0 && loaded < 150.0, "the bristles take up a force below breakaway");
    for (int i = 0; i < 1000; ++i)
    {
        RequireNear(friction.Evaluate(0.0, 0.0, 1e-3), loaded, 1e-9, "the force held at rest");
        friction.Commit(0.0, 0.0, 1e-3);
    }
    StribeckParameters sp = p.stribeck;
    RequireNear(StribeckFriction(sp).Evaluate(0.0, 0.0, 1e-3), 0.0, 1e-12, "static Stribeck holds nothing at rest");
}

StrutUnit MakeStrut(double breakaway, double mountStiffness, double mountDamping)
{
    StrutUnitSettings s;
    s.springPreload = 3500.0;
    s.coilSpring = Curve::Linear(30000.0);
    s.damper = Curve::Table({-1.0, 0.0, 1.0}, {-2000.0, 0.0, 1500.0});
    s.damperMount = Curve::Linear(mountStiffness);
    s.damperMountDamping = mountDamping;
    LuGreParameters p;
    p.stribeck.coulomb = {120.0, 0.0, 0.0};
    p.stribeck.breakaway = {breakaway, 0.0, 0.0};
    p.stribeck.stribeckVelocity = 0.003;
    p.bristleStiffness = 2e6;
    p.bristleDamping = 1500.0;
    return StrutUnit(s, std::make_unique<LuGreFriction>(p));
}

int CountForceDrops(StrutUnit strut, double rate, double seconds, double minimumDrop)
{
    double compression = 0.0;
    double previous = 0.0;
    double peak = -1e300;
    bool falling = false;
    int drops = 0;
    for (int i = 0; i < static_cast<int>(seconds * 1000.0); ++i)
    {
        compression += rate * 1e-3;
        strut.Step(compression, rate, 0.0, 1e-3);
        const double rod = strut.DamperPathForce();
        if (const char* trace = std::getenv("MINIENGINE_STICK_SLIP_TRACE"))
        {
            static std::ofstream file(trace);
            file << i << ',' << rod << ',' << strut.PistonVelocity() << '\n';
        }
        if (i > 50)
        {
            if (rod < previous && !falling)
            {
                peak = previous;
                falling = true;
            }
            if (falling && rod > previous)
            {
                if (peak - previous > minimumDrop)
                {
                    ++drops;
                }
                falling = false;
            }
        }
        previous = rod;
    }
    return drops;
}

void TestTopMountLetsTheRodStickAndSlip()
{
    // Slow compression (2 mm/s) through a soft damper-path mount: the rod sticks until the mount
    // has loaded the seal past breakaway, then slips and the force drops; again and again.
    const int stickSlip = CountForceDrops(MakeStrut(400.0, 1.5e5, 30.0), 0.002, 4.0, 40.0);
    // No Stribeck drop (breakaway = Coulomb): no stick-slip cycles.
    const int smooth = CountForceDrops(MakeStrut(120.0, 1.5e5, 30.0), 0.002, 4.0, 40.0);
    std::cout << "  rod force drops over 4 s: " << stickSlip << " with a Stribeck drop, " << smooth << " without\n";
    Require(stickSlip >= 3, "stick-slip appears with a compliant mount and a Stribeck drop");
    Require(smooth == 0, "and not without the drop");
}

// A coil loose in its seats: preloaded 3000 N at 30 N/mm it unloads 100 mm out, and further out it is
// slack (no force, no stiffness) rather than pulling; a held coil pulls on.
void TestLooseSpringGoesSlack()
{
    StrutUnitSettings s;
    s.springPreload = 3000.0;
    s.coilSpring = Curve::Linear(30000.0);
    s.springPushesOnly = true;
    StrutUnit loose(s, std::make_unique<NoFriction>());
    s.springPushesOnly = false;
    StrutUnit held(s, std::make_unique<NoFriction>());
    RequireNear(loose.Step(-0.05, 0.0, 0.0, 1e-3), 1500.0, 1e-9, "half way out the loose coil still pushes");
    RequireNear(loose.StiffnessSlope(-0.05), 30000.0, 1e-9, "with its rate");
    RequireNear(loose.Step(-0.12, 0.0, 0.0, 1e-3), 0.0, 1e-9, "past where it unloads it is slack");
    RequireNear(loose.StiffnessSlope(-0.12), 0.0, 1e-9, "and has no rate");
    RequireNear(held.Step(-0.12, 0.0, 0.0, 1e-3), -600.0, 1e-9, "a held coil pulls");
}

// Assetto Corsa's [AXLE] links (x left, y up, z forward from the axle's centre) in the axle frame
// (forward, left, up).
AxleLinkDef AcLink(Vec3 car, Vec3 axle)
{
    return AxleLinkDef{Vec3(car.z, car.x, car.y), Vec3(axle.z, axle.x, axle.y)};
}

// The AE86's rear axle (ks_toyota_ae86): four trailing links and a Panhard rod, 1.35 m track, springs
// of 22 kN/m at 72 % of the half axle, 0.29 m tyres.
SolidAxleDefinition Ae86Axle()
{
    SolidAxleDefinition def;
    def.links = {
        AcLink(Vec3(0.4933, -0.020, 0.498), Vec3(0.4900, -0.080, 0.0)),
        AcLink(Vec3(-0.4933, -0.020, 0.498), Vec3(-0.4900, -0.080, 0.0)),
        AcLink(Vec3(0.2488, 0.075, 0.2405), Vec3(0.2488, 0.020, 0.0)),
        AcLink(Vec3(-0.2488, 0.075, 0.2405), Vec3(-0.2488, 0.020, 0.0)),
        AcLink(Vec3(-0.435, 0.010, -0.100), Vec3(0.435, -0.070, -0.110)),
    };
    def.track = 1.35;
    def.tyreRadius = 0.29;
    def.springPosition = 0.72;
    return def;
}

StrutUnit AxleUnit(double preload)
{
    StrutUnitSettings s;
    s.springPreload = preload;
    s.coilSpring = Curve::Linear(22000.0);
    s.damper = Curve::Linear(2000.0);
    return StrutUnit(s, std::make_unique<NoFriction>());
}

// A solid axle at rest, bouncing and rolling: it rises without changing camber, it rolls the wheels
// with it (camber to the body = its roll), its Panhard rod sets the roll centre, and the redundant
// fifth link only asks the bushings for fractions of a millimetre.
void TestSolidAxleMovesAsOnePiece()
{
    SolidAxle axle(Ae86Axle(), AxleUnit(3000.0), AxleUnit(3000.0));
    RequireNear(axle.LinkStretch(), 0.0, 1e-9, "the axle sits on its links at the design position");
    KinematicOutputs left;
    KinematicOutputs right;

    axle.Solve(0.04, 0.04);
    axle.ComputeOutputs(0, left);
    axle.ComputeOutputs(1, right);
    RequireNear(axle.Pose().center.z, 0.04, 1e-12, "bounce lifts the axle's centre");
    RequireNear(axle.Pose().roll, 0.0, 1e-9, "without rolling it");
    RequireNear(left.camber, 0.0, 1e-6, "bounce keeps the camber");
    RequireNear(left.wheelCenter.z, 0.04, 1e-9, "the wheel centre follows its travel");
    std::cout << "AE86 axle at 40 mm bump: centre moves " << axle.Pose().center.x * 1000.0 << " mm forward, " << axle.Pose().center.y * 1000.0
              << " mm left (Panhard arc), pitches " << axle.Pose().pitch * 180.0 / 3.14159265 << " deg; links give " << axle.LinkStretch() * 1000.0 << " mm\n";
    Require(axle.LinkStretch() < 1e-4, "bounce suits the links");

    axle.Solve(0.03, -0.03);
    axle.ComputeOutputs(0, left);
    axle.ComputeOutputs(1, right);
    // The wheel centres rise by their travel: sin(roll) = 60 mm over the track (the axle's small pitch aside).
    const double roll = std::asin(0.06 / 1.35);
    RequireNear(axle.Pose().roll, roll, 1e-4, "the axle rolls by the travel difference over the track");
    RequireNear(left.camber, -roll, 1e-4, "the rising wheel's top leans in");
    RequireNear(right.camber, roll, 1e-4, "the falling wheel's top leans out");
    std::cout << "AE86 axle rolled 2.5 deg: links give " << axle.LinkStretch() * 1000.0 << " mm, roll steer " << left.toe * 180.0 / 3.14159265 << " deg\n";
    Require(axle.LinkStretch() < 1e-3, "the redundant link binds by under a millimetre");

    // The roll centre: where the Panhard rod crosses the centre plane (30 mm under the wheel centres),
    // moved by the axle's roll steer: the rod sits 0.11 m behind the wheels, so a yaw rate r per unit
    // roll rate shifts the point in the wheels' plane by 0.11 r.
    axle.Solve(0.0, 0.0);
    axle.ComputeOutputs(0, left);
    const double rod = -0.070 + (0.010 - (-0.070)) * 0.435 / 0.870;
    axle.Solve(0.001, -0.001);
    const double yawPerRoll = axle.Pose().yaw / axle.Pose().roll;
    axle.Solve(0.0, 0.0);
    const double expected = 0.29 + rod + 0.110 * yawPerRoll;
    std::cout << "AE86 axle roll centre " << left.rollCenterHeight * 1000.0 << " mm up: the Panhard rod at the centre " << (0.29 + rod) * 1000.0
              << " mm, moved by the roll steer to " << expected * 1000.0 << " mm\n";
    RequireNear(left.rollCenterHeight, expected, 0.002, "the roll centre from the Panhard rod and the roll steer");
}

// Assetto Corsa's 250 GTO: four parallel trailing links, the leaf springs holding the axle sideways.
// Rolling, it turns about its own centre.
void TestLeafSprungAxleRollsAboutItsCentre()
{
    SolidAxleDefinition def;
    def.links = {
        AcLink(Vec3(0.4588, -0.102, 0.441), Vec3(0.458, -0.082, -0.031)),
        AcLink(Vec3(-0.4588, -0.102, 0.441), Vec3(-0.458, -0.082, -0.031)),
        AcLink(Vec3(0.4588, 0.060, 0.441), Vec3(0.458, 0.076, 0.031)),
        AcLink(Vec3(-0.4588, 0.060, 0.441), Vec3(-0.458, 0.076, 0.031)),
    };
    def.track = 1.4;
    def.tyreRadius = 0.32;
    def.lateralStiffness = 350000.0;
    SolidAxle axle(def, AxleUnit(3000.0), AxleUnit(3000.0));
    KinematicOutputs left;
    axle.ComputeOutputs(0, left);
    RequireNear(left.rollCenterHeight, 0.32, 0.01, "the leaf springs hold the axle's centre: the roll centre at the wheel centres");
    axle.Solve(0.03, -0.03);
    RequireNear(axle.Pose().center.y, 0.0, 1e-4, "the axle stays centred");
}

// The units at 72 % of the half axle: bounce meets the spring rate at the wheel, roll only 0.72 squared
// of it (the analytic 2 k a^2 h^2 roll stiffness of the springs).
void TestSolidAxleSpringsAtTheirPlace()
{
    SolidAxle axle(Ae86Axle(), AxleUnit(0.0), AxleUnit(0.0));
    std::array<CornerInput, 2> in{};
    in[0].travel = in[1].travel = 0.01;
    axle.Step(in);
    RequireNear(axle.Output(0).strutTravelForce, -220.0, 1e-6, "bounce: the spring's rate at the wheel");
    in[0].travel = 0.01;
    in[1].travel = -0.01;
    axle.Step(in);
    RequireNear(axle.Output(0).strutTravelForce, -22000.0 * 0.72 * 0.72 * 0.01, 1e-6, "roll: the rate times 0.72 squared");
    RequireNear(axle.Output(1).strutTravelForce, 22000.0 * 0.72 * 0.72 * 0.01, 1e-6, "and the other way on the other wheel");
}

// Rill's double wishbone as five rods: each arm split into its front and rear rod, meeting at the arm's
// ball joint, and the tie rod. The linkage is the same, so is every curve: camber, toe and half-track
// over the travel, and the steering.
void TestFiveLinkSplitWishboneIsTheWishbone()
{
    const SuspensionDefinition dwb = RillDoubleWishbone();
    const auto at = [&](const char* name) {
        for (const PointDef& p : dwb.points)
        {
            if (p.name == name)
            {
                return p.position;
            }
        }
        throw std::runtime_error(std::string("no point ") + name);
    };
    FiveLinkHardpoints hp;
    hp.chassis = {at("lower_front"), at("lower_rear"), at("upper_front"), at("upper_rear"), at("tie_inner")};
    hp.knuckle = {at("lower_ball"), at("lower_ball"), at("upper_ball"), at("upper_ball"), at("tie_outer")};
    hp.steerLink = 4;
    hp.wheelCenter = at("wheel_center");
    const SuspensionDefinition five = MakeFiveLink(hp, dwb.tyreRadius, dwb.wheelAxis);
    Kinematics a(Compile(dwb));
    Kinematics b(Compile(five));
    double worst = 0.0;
    for (int s = 0; s <= 16; ++s)
    {
        const double z = -0.08 + 0.01 * s;
        for (const double rack : {-0.02, 0.0, 0.02})
        {
            a.Solve(z, rack);
            b.Solve(z, rack);
            KinematicOutputs oa;
            KinematicOutputs ob;
            ComputeOutputs(a, oa);
            ComputeOutputs(b, ob);
            worst = std::max({worst, std::abs(oa.camber - ob.camber), std::abs(oa.toe - ob.toe), std::abs(oa.halfTrackChange - ob.halfTrackChange)});
        }
    }
    RequireNear(worst, 0.0, 1e-9, "the split wishbone moves as the wishbone");
}

// A five-link with all its joints apart (a typical rear multi-link: two lower rods, two short upper rods
// rising outboard as the GT-R's upper arm does, and a toe link, unsteered) solves across its travel; the
// rising upper rods steepen as the wheel rises and pull its top in.
void TestFiveLinkSolvesAcrossItsTravel()
{
    FiveLinkHardpoints hp;
    hp.chassis = {Vec3(0.20, 0.30, -0.10), Vec3(-0.15, 0.30, -0.12), Vec3(0.12, 0.40, 0.08), Vec3(-0.18, 0.42, 0.07), Vec3(-0.25, 0.35, -0.02)};
    hp.knuckle = {Vec3(0.06, 0.70, -0.12), Vec3(-0.05, 0.71, -0.13), Vec3(0.04, 0.66, 0.16), Vec3(-0.05, 0.66, 0.15), Vec3(-0.14, 0.69, -0.02)};
    hp.wheelCenter = Vec3(0.0, 0.76, 0.0);
    hp.steered = false;
    SuspensionDefinition def = MakeFiveLink(hp, 0.32, Vec3(0.0, 1.0, 0.0));
    def.vehicleCenter = Vec3(0.0, 0.0, 0.0);
    Kinematics k(Compile(def));
    KinematicOutputs out;
    for (int s = 0; s <= 12; ++s)
    {
        const double z = -0.06 + 0.01 * s;
        Require(k.Solve(z, 0.0).status == SolveStatus::Converged, "the five-link solves at " + std::to_string(z));
    }
    k.Solve(0.0, 0.0);
    ComputeOutputs(k, out);
    std::cout << "five-link: camber gain " << out.camberPerTravel / kDeg << " deg/m, bump steer " << out.toePerTravel / kDeg << " deg/m, roll centre "
              << out.rollCenterHeight * 1000.0 << " mm\n";
    Require(out.camberPerTravel < 0.0, "the upper rods shorter: the wheel's top leans in as it rises");
}

void TestSpringPathSeriesRubber()
{
    StrutUnitSettings s;
    s.springPreload = 0.0;
    s.coilSpring = Curve::Linear(30000.0);
    s.springMount = Curve::Linear(120000.0);
    StrutUnit strut(s, std::make_unique<NoFriction>());
    const double force = strut.Step(0.01, 0.0, 0.0, 1e-3);
    // Series: k = 1 / (1/30k + 1/120k) = 24 kN/m.
    RequireNear(force, 240.0, 1e-6, "coil and seat rubber in series");
    RequireNear(strut.SpringMountDeflection(), 0.002, 1e-9, "the rubber takes its share");
}

void TestCornerStepsAtOneKilohertz()
{
    SuspensionCorner corner(BoxsterStrut(true), MakeStrut(250.0, 4e5, 300.0), 0, 0);
    CornerInput in;
    double worstResidual = 0.0;
    for (int i = 1; i <= 2000; ++i)
    {
        const double t = i * 1e-3;
        in.travel = 0.05 * std::sin(2.0 * kPi * 1.5 * t);
        in.travelRate = 0.05 * 2.0 * kPi * 1.5 * std::cos(2.0 * kPi * 1.5 * t);
        in.rack = 0.02 * std::sin(2.0 * kPi * 0.5 * t);
        in.rackRate = 0.02 * 2.0 * kPi * 0.5 * std::cos(2.0 * kPi * 0.5 * t);
        in.load.force = {-2000.0 * std::sin(2.0 * kPi * 0.7 * t), 1500.0, 4000.0};
        const CornerOutput& out = corner.Step(in);
        Require(out.solve.status == SolveStatus::Converged, "corner kinematics converge");
        Require(std::isfinite(out.strutForce) && std::isfinite(out.travelForce) && std::isfinite(out.nextSideLoad), "finite outputs");
        worstResidual = std::max(worstResidual, out.solve.residual);
    }
    Require(worstResidual < 1e-10, "corner residual");
}

// A symmetric car on Rill's double wishbone at all four corners, with rates at the wheel: 1200 kg,
// 2.6 m wheelbase, 50/50, 40 kN/m wheels, 200 kN/m tyres, 40 kg hubs.
CarModel RigTestCar(double damping, double antiRollBar)
{
    CarModel car;
    car.name = "rig test car";
    car.mass = 1200.0;
    car.wheelbase = 2.6;
    car.cgHeight = 0.5;
    car.frontBrakeShare = 0.6;
    for (int i = 0; i < 4; ++i)
    {
        CarCorner& c = car.corners[i];
        const SuspensionDefinition left = RillDoubleWishbone();
        c.definition = i % 2 == 0 ? left : MirrorToRight(left);
        c.definition.steered = i < 2;
        c.unit.coilSpring = Curve::Linear(40000.0);
        c.unit.damper = Curve::Linear(damping);
        c.antiRollBarRate = antiRollBar;
        c.hubMass = 40.0;
        c.tyreRate = 200000.0;
        c.tyreDamping = 0.0;
        c.position = Vec3(i < 2 ? car.wheelbase : 0.0, i % 2 == 0 ? 0.768 : -0.768, 0.0);
    }
    BalanceCar(car, 0.5);
    car.rollInertia = 450.0;
    car.pitchInertia = 1500.0;
    car.rackAtLock = 0.06;
    car.steeringWheelLockDegrees = 450.0;
    return car;
}

void TestRillSweepParameters()
{
    // Rill 2012, p. 179: 1 Hz to 2 Hz in N + 1 = 4 cycles: q = 0.2310, q/p = 0.2063, 2.9237 s.
    const SineSweep sweep(1.0, 2.0, 3);
    RequireNear(sweep.q, 0.2310, 1e-4, "sweep q");
    RequireNear(sweep.q / sweep.p, 0.2063, 1e-4, "sweep q/p");
    RequireNear(sweep.Duration(), 2.9237, 1e-4, "sweep duration");
    RequireNear(1.0 / (sweep.CycleStart(1) - sweep.CycleStart(0)), 1.0, 1e-9, "first cycle at f0");
}

void TestSevenPostRigRestsAndResonatesWhereItShould()
{
    // At rest it stays at rest: the springs' preloads carry the body, the tyres the whole car.
    const CarModel car = RigTestCar(1500.0, 0.0);
    SevenPostRig rig(car);
    const std::array<double, 4> zero{};
    double loads = 0.0;
    for (int i = 0; i < 2000; ++i)
    {
        rig.Step(zero, zero, 0.0, 0.0, 0.0, 1e-3);
    }
    for (int i = 0; i < 4; ++i)
    {
        loads += rig.TyreLoad(i);
    }
    RequireNear(rig.Heave(), 0.0, 1e-6, "the rig at rest stays put");
    RequireNear(loads, car.mass * 9.81, 1e-3, "the tyres carry the car");

    // Heave sweep, lightly damped: the body resonates at the ride rate's frequency and the hubs at
    // theirs (quarter-car estimates: springs and tyres in series for the body, in parallel for the hub).
    const SweepResult heave = RunSweep(car, RigMode::Heave, SineSweep(0.5, 25.0, 120), 0.002, 0.05);
    const double ride = 40000.0 * 200000.0 / (40000.0 + 200000.0);
    const double body = std::sqrt(4.0 * ride / car.sprungMass) / (2.0 * kPi);
    const double hop = std::sqrt((40000.0 + 200000.0) / 40.0) / (2.0 * kPi);
    std::cout << "  seven-post heave: body " << heave.bodyFrequency << " Hz (estimate " << body << "), wheel hop " << heave.wheelHopFrequency
              << " Hz (estimate " << hop << "), damping " << heave.bodyDamping << "\n";
    RequireNear(heave.bodyFrequency, body, 0.08 * body, "heave resonance");
    RequireNear(heave.wheelHopFrequency, hop, 0.1 * hop, "wheel hop");
    // The damper's ratio at the body (quarter car, ride rate): c / (2 sqrt(k m)).
    const double zeta = 1500.0 / (2.0 * std::sqrt(ride * car.sprungMass / 4.0));
    RequireNear(heave.bodyDamping, zeta, 0.5 * zeta, "half-power damping estimate");
}

void TestGameHubSchemeAtTheGameStep()
{
    // The game's hub scheme (PhysicsWorld::StepUnsprungCorner) at its 1 ms step against a 50 us
    // reference, on a lightly damped car whose hubs hop (hub damping ratio about 0.24). The trapezoidal
    // hub step leaves the wheel hop's peak within a few percent (backward Euler's numerical damping took
    // some 15 % off it); what error is left comes from the coupling frozen at the step's start and is
    // first order in the step. Measured 2026-10-04 (docs/design/2026-10-04-unsprung-corner-integration-notes.md).
    const CarModel car = RigTestCar(1500.0, 0.0);
    {
        SevenPostRig rig(car, true, UnsprungScheme::GameLinearlyImplicit);
        const std::array<double, 4> zero{};
        for (int i = 0; i < 2000; ++i)
        {
            rig.Step(zero, zero, 0.0, 0.0, 0.0, 1e-3);
        }
        RequireNear(rig.Heave(), 0.0, 1e-6, "the game scheme at rest stays put");
    }
    const SineSweep sweep(2.0, 20.0, 30);
    const SweepResult reference = RunSweep(car, RigMode::Heave, sweep, 0.002, 0.05, true, 5e-5);
    const SweepResult game = RunSweep(car, RigMode::Heave, sweep, 0.002, 0.05, true, 1e-3, UnsprungScheme::GameLinearlyImplicit);
    const SweepResult quarter = RunSweep(car, RigMode::Heave, sweep, 0.002, 0.05, true, 2.5e-4, UnsprungScheme::GameLinearlyImplicit);
    Require(game.cycles.size() == reference.cycles.size() && quarter.cycles.size() == reference.cycles.size(), "same sweep cycles");
    double hopWheelError = 0.0;
    // The load variation's error against its peak over the sweep: in the trough between the body's
    // resonance and the hop it is a few per cent of the static load, where a relative error means little.
    double peakLoadVariation = 0.0;
    for (const SweepCycle& r : reference.cycles)
    {
        peakLoadVariation = std::max(peakLoadVariation, r.loadVariation[0]);
    }
    struct Errors
    {
        double wheel = 0.0;
        double load = 0.0;
        double body = 0.0;
    };
    Errors game1ms;
    Errors game250us;
    const auto track = [&](Errors& e, const SweepCycle& c, const SweepCycle& r)
    {
        e.wheel = std::max(e.wheel, std::abs(c.wheelGain[0] / r.wheelGain[0] - 1.0));
        e.load = std::max(e.load, std::abs(c.loadVariation[0] - r.loadVariation[0]) / peakLoadVariation);
        e.body = std::max(e.body, std::abs(c.bodyGain / r.bodyGain - 1.0));
    };
    for (std::size_t i = 0; i < reference.cycles.size(); ++i)
    {
        const SweepCycle& r = reference.cycles[i];
        track(game1ms, game.cycles[i], r);
        track(game250us, quarter.cycles[i], r);
        if (std::abs(r.frequency - reference.wheelHopFrequency) < 1e-9)
        {
            hopWheelError = game.cycles[i].wheelGain[0] / r.wheelGain[0] - 1.0;
        }
    }
    std::cout << "  game hub scheme at 1 ms: wheel " << 100.0 * game1ms.wheel << " %, load " << 100.0 * game1ms.load << " % of its peak, body " << 100.0 * game1ms.body
              << " %, wheel at the hop " << 100.0 * hopWheelError << " %; body at 0.25 ms " << 100.0 * game250us.body << " %\n";
    // The hub's own motion: the trapezoidal step leaves it within a few per cent everywhere.
    Require(game1ms.wheel < 0.04, "the hub's motion within 4 %");
    Require(std::abs(hopWheelError) < 0.04, "the wheel hop's peak within 4 %");
    Require(game1ms.load < 0.04, "the tyre's load within 4 % of its peak variation");
    // The body: the coupling frozen at the step's start (its acceleration a step late) takes a few per cent,
    // first order in the step.
    Require(game1ms.body < 0.08, "the body within 8 %");
    Require(game250us.body < 0.4 * game1ms.body, "converging at first order");
}

void TestKcRigMeasuresTheSpringsAndTheLinkage()
{
    const CarModel car = RigTestCar(1500.0, 20000.0);
    const KcResult kc = RunKcRig(car);
    const double track = 2.0 * 0.768;
    // Roll stiffness of an axle: (k + 2 k_arb) t^2 / 2 per radian.
    const double expected = (40000.0 + 2.0 * 20000.0) * track * track / 2.0 * kDeg;
    std::cout << "  K&C: roll stiffness " << kc.axles[0].rollStiffness << " Nm/deg (springs+bar " << expected << "), wheel rate "
              << kc.axles[0].wheelRate << " N/mm, RC " << kc.axles[0].rollCenterHeight << " mm, ratio " << kc.steeringRatio << ", anti-dive "
              << kc.antiDiveFront << " %\n";
    RequireNear(kc.axles[0].rollStiffness, expected, 0.03 * expected, "roll stiffness from springs and bar");
    RequireNear(kc.axles[0].wheelRate, 40.0, 1.0, "wheel rate");
    // The linkage's numbers are those of the single corner (Rill's: roll centre 86 mm, kingpin 10.5).
    RequireNear(kc.axles[0].rollCenterHeight, 86.0, 1.0, "roll centre height");
    RequireNear(kc.axles[0].kingpinInclination, 10.5182, 1e-3, "kingpin inclination");
    RequireNear(kc.rollStiffnessFrontShare, 0.5, 1e-6, "same axles share the roll stiffness");
    // Steering: both wheels turn the way the wheel does, near-Ackermann, a car's ratio.
    const KcSteerPoint& full = kc.steer.back();
    Require(full.left > 5.0 && full.right > 5.0, "full lock turns both wheels right");
    Require(kc.steeringRatio > 5.0 && kc.steeringRatio < 40.0, "a car's steering ratio");
}

template <typename F>
double TimePerCall(F&& f, int calls)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i)
    {
        f(i);
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count() / calls;
}

void TestTimingBudget()
{
    const int calls = 20000;
    for (const SuspensionDefinition& def : {RillDoubleWishbone(), BoxsterStrut(true)})
    {
        const Model model = Compile(def);
        Kinematics kin(model);
        KinematicOutputs out;
        Reactions reactions;
        WheelLoad load;
        load.force = {-1000.0, 1000.0, 4000.0};
        const double solve = TimePerCall([&](int i) {
            const double t = i * 1e-3;
            kin.Solve(0.06 * std::sin(2.0 * kPi * 2.0 * t), 0.02 * std::sin(2.0 * kPi * 0.5 * t));
        }, calls);
        const double outputs = TimePerCall([&](int) {
            ComputeOutputs(kin, out);
        }, calls / 4);
        const double statics = TimePerCall([&](int) {
            SolveReactions(kin, load, {3000.0}, reactions);
        }, calls / 4);
        SuspensionCorner corner(def, MakeStrut(250.0, 4e5, 300.0), 0, def.sliders.empty() ? -1 : 0);
        CornerInput in;
        in.load = load;
        const double step = TimePerCall([&](int i) {
            const double t = i * 1e-3;
            in.travel = 0.06 * std::sin(2.0 * kPi * 2.0 * t);
            in.travelRate = 0.06 * 2.0 * kPi * 2.0 * std::cos(2.0 * kPi * 2.0 * t);
            in.rack = 0.02 * std::sin(2.0 * kPi * 0.5 * t);
            corner.Step(in);
        }, calls);
        SuspensionDefinition bushed = def;
        for (const char* name : {"lower_front", "lower_rear"})
        {
            BushingDef b;
            b.point = name;
            b.x = Curve::Progressive(1e6, 1e7, 0.002);
            b.y = Curve::Progressive(4e6, 4e7, 0.001);
            b.z = b.y;
            bushed.bushings.push_back(b);
        }
        Compliance compliance(Compile(bushed, ModelMode::Compliant));
        const double compliant = TimePerCall([&](int i) {
            load.force.y = 1000.0 + 500.0 * std::sin(i * 0.01);
            compliance.Solve(0.0, 0.0, load, {3000.0});
        }, calls / 10);
        std::cout << "  " << def.name << " (" << model.unknowns << " unknowns): solve " << solve << " us, outputs " << outputs
                  << " us, reactions " << statics << " us, full corner step " << step << " us, compliance solve " << compliant << " us\n";
#ifdef NDEBUG
        Require(step < 50.0, def.name + ": a corner step must fit well inside a 1 ms frame");
#endif
    }
}

// Writes K&C-style curves (travel sweep and rack sweep) for plotting against other tools.
void WriteCurves(const std::string& directory)
{
    struct Named
    {
        std::string file;
        SuspensionDefinition def;
    };
    for (const Named& n : {Named{"rill_double_wishbone", RillDoubleWishbone()}, Named{"boxster_front_strut", BoxsterStrut(true)}, Named{"boxster_rear_strut", BoxsterStrut(false)}})
    {
        std::ofstream file(directory + "/" + n.file + "_travel.csv");
        file << "travel_mm,camber_deg,toe_deg,camber_gain_deg_per_m,bump_steer_deg_per_m,half_track_change_mm,wheelbase_change_mm,kpi_deg,caster_deg,scrub_mm,trail_mm,roll_center_mm,contact_path_deg,center_path_deg,motion_ratio\n";
        Kinematics kin(Compile(n.def));
        for (int i = 0; i >= -100; --i)
        {
            kin.Solve(i * 1e-3, 0.0);
        }
        for (int i = -100; i <= 100; i += 2)
        {
            if (kin.Solve(i * 1e-3, 0.0).status != SolveStatus::Converged)
            {
                continue;
            }
            KinematicOutputs o;
            ComputeOutputs(kin, o);
            file << i << ',' << o.camber / kDeg << ',' << o.toe / kDeg << ',' << o.camberPerTravel / kDeg << ',' << o.toePerTravel / kDeg << ','
                 << o.halfTrackChange * 1e3 << ',' << o.wheelbaseChange * 1e3 << ',' << o.kingpinInclination / kDeg << ',' << o.caster / kDeg << ','
                 << o.scrubRadius * 1e3 << ',' << o.casterTrail * 1e3 << ',' << o.rollCenterHeight * 1e3 << ',' << o.contactPathAngle / kDeg << ','
                 << o.wheelCenterPathAngle / kDeg << ',' << (o.elements.empty() ? 0.0 : o.elements[0].motionRatio) << '\n';
        }
    }
    std::cout << "  curves written to " << directory << "\n";
}
}

int main()
{
    try
    {
        TestModelsAreDeterminate();
        TestRillSteeringGeometryMatchesTheBook();
        TestRollCenterAgreesWithTheContactPath();
        TestCurvesMatchClosedFormSolutions();
        TestMirroredCornerReportsTheSame();
        TestNewtonNeedsOneOrTwoCorrections();
        TestSensitivitiesAndAccelerations();
        TestOutOfReachIsClampedAndRecovers();
        TestReactionsBalanceTheLoad();
        TestStrutSideLoadFollowsBraking();
        TestComplianceIsNonlinearWhereTheBushingsAre();
        TestLuGreSteadyStateIsTheStribeckCurve();
        TestLuGreSticksBelowBreakaway();
        TestSpringPathSeriesRubber();
        TestLooseSpringGoesSlack();
        TestSolidAxleMovesAsOnePiece();
        TestLeafSprungAxleRollsAboutItsCentre();
        TestSolidAxleSpringsAtTheirPlace();
        TestFiveLinkSplitWishboneIsTheWishbone();
        TestFiveLinkSolvesAcrossItsTravel();
        TestTopMountLetsTheRodStickAndSlip();
        TestCornerStepsAtOneKilohertz();
        TestRillSweepParameters();
        TestSevenPostRigRestsAndResonatesWhereItShould();
        TestGameHubSchemeAtTheGameStep();
        TestKcRigMeasuresTheSpringsAndTheLinkage();
        TestTimingBudget();
        if (const char* directory = std::getenv("MINIENGINE_SUSPENSION_CSV"))
        {
            WriteCurves(directory);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "suspension tests failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "suspension tests passed\n";
    return 0;
}
