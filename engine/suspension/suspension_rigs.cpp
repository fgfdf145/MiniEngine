#include "suspension_rigs.h"

#include "suspension_kinematics.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>

namespace me::suspension
{

namespace
{
constexpr double kGravity = 9.81;
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

// The pads' pattern for a mode: FL, FR, RL, RR.

// The body's displacement that a mode moves, in metres at the wheels.
double ModeBody(const SevenPostRig& rig, RigMode mode)
{
    const CarModel& car = rig.Car();
    switch (mode)
    {
    case RigMode::Heave:
    case RigMode::Warp:
        return rig.Heave();
    case RigMode::Pitch:
        return rig.Pitch() * 0.5 * car.wheelbase;
    case RigMode::Roll:
        // Left pads up roll the body's right side down: positive roll.
        return rig.Roll() * std::abs(car.corners[0].position.y);
    }
    return 0.0;
}

double ModeBodyAccel(const SevenPostRig& rig, RigMode mode)
{
    const CarModel& car = rig.Car();
    switch (mode)
    {
    case RigMode::Heave:
    case RigMode::Warp:
        return rig.HeaveAcceleration();
    case RigMode::Pitch:
        return rig.PitchAcceleration() * 0.5 * car.wheelbase;
    case RigMode::Roll:
        return rig.RollAcceleration() * std::abs(car.corners[0].position.y);
    }
    return 0.0;
}

// First harmonic of samples over one cycle whose phase runs through 2 pi: amplitude and phase.
struct Harmonic
{
    double sine = 0.0;
    double cosine = 0.0;
    double Amplitude() const
    {
        return std::hypot(sine, cosine);
    }
    double Phase() const
    {
        return std::atan2(cosine, sine);
    }
};

Harmonic FirstHarmonic(const std::vector<double>& values, const std::vector<double>& phases)
{
    Harmonic h;
    if (values.empty())
    {
        return h;
    }
    double mean = 0.0;
    for (double v : values)
    {
        mean += v;
    }
    mean /= static_cast<double>(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        h.sine += (values[i] - mean) * std::sin(phases[i]);
        h.cosine += (values[i] - mean) * std::cos(phases[i]);
    }
    h.sine *= 2.0 / static_cast<double>(values.size());
    h.cosine *= 2.0 / static_cast<double>(values.size());
    return h;
}

// The road's push on a corner that holds its travel against the unit and the anti-roll bar.
double PadForce(const CornerOutput& out, double antiRollBar)
{
    return -(out.strutTravelForce + antiRollBar) / std::max(out.normalPerTravel, 0.2);
}

double WrapDegrees(double degrees)
{
    while (degrees > 180.0)
    {
        degrees -= 360.0;
    }
    while (degrees < -180.0)
    {
        degrees += 360.0;
    }
    return degrees;
}
}

StrutUnit MakeCornerUnit(const CarCorner& corner)
{
    if (corner.hasFriction)
    {
        return StrutUnit(corner.unit, std::make_unique<LuGreFriction>(corner.friction));
    }
    return StrutUnit(corner.unit, std::make_unique<NoFriction>());
}

std::unique_ptr<AxleSuspension> MakeAxleSuspension(const CarModel& car, int axle, bool friction)
{
    CarCorner left = car.corners[2 * axle];
    CarCorner right = car.corners[2 * axle + 1];
    left.hasFriction = left.hasFriction && friction;
    right.hasFriction = right.hasFriction && friction;
    if (car.solidAxles[axle].has_value())
    {
        return std::make_unique<AxleSuspension>(std::make_unique<SolidAxle>(*car.solidAxles[axle], MakeCornerUnit(left), MakeCornerUnit(right)));
    }
    return std::make_unique<AxleSuspension>(
        std::make_unique<SuspensionCorner>(left.definition, MakeCornerUnit(left), left.unitElement, left.slider),
        std::make_unique<SuspensionCorner>(right.definition, MakeCornerUnit(right), right.unitElement, right.slider));
}

void BalanceCar(CarModel& car, double frontAxleShareOfWeight)
{
    const double weight = car.mass * kGravity;
    double hubs = 0.0;
    for (const CarCorner& c : car.corners)
    {
        hubs += c.hubMass;
    }
    car.sprungMass = car.mass - hubs;
    double sum = 0.0;
    double moment = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        CarCorner& corner = car.corners[i];
        const double axle = i < 2 ? frontAxleShareOfWeight : 1.0 - frontAxleShareOfWeight;
        car.staticLoad[i] = 0.5 * weight * axle - corner.hubMass * kGravity;
        // The unit's preload: the load over its motion ratio when it sits in the linkage.
        double ratio = 1.0;
        if (corner.unitElement != SuspensionCorner::kWheelTravel && !car.solidAxles[i / 2].has_value())
        {
            Kinematics kinematics(Compile(corner.definition));
            KinematicOutputs out;
            ComputeOutputs(kinematics, out);
            ratio = std::max(out.elements[corner.unitElement].motionRatio, 0.05);
        }
        corner.unit.springPreload = car.staticLoad[i] / ratio;
        sum += car.staticLoad[i];
        moment += car.staticLoad[i] * corner.position.x;
    }
    // The sprung mass's centre is where the springs' loads balance.
    const double cgX = moment / sum;
    for (CarCorner& corner : car.corners)
    {
        corner.position.x -= cgX;
    }
}

KcResult RunKcRig(const CarModel& car, double bounceRange, double rollRange, double rackFraction)
{
    KcResult result;
    const int bounceSteps = 24;
    const int rollSteps = 12;

    // An axle's wheels walked to a travel each and a rack in small steps (as the rig's actuators
    // move), at rest.
    struct Walk
    {
        std::unique_ptr<AxleSuspension> axle;
        std::array<double, 2> travel{};
        double rack = 0.0;
    };
    const auto settle = [&](Walk& walk, double toLeft, double toRight, double toRack) {
        const int steps = 10;
        for (int s = 1; s <= steps; ++s)
        {
            std::array<CornerInput, 2> in{};
            in[0].travel = walk.travel[0] + (toLeft - walk.travel[0]) * s / steps;
            in[1].travel = walk.travel[1] + (toRight - walk.travel[1]) * s / steps;
            in[0].rack = in[1].rack = walk.rack + (toRack - walk.rack) * s / steps;
            walk.axle->Step(in);
        }
        walk.travel = {toLeft, toRight};
        walk.rack = toRack;
        return std::array<CornerOutput, 2>{walk.axle->Output(0), walk.axle->Output(1)};
    };
    const auto wheel = [](const CornerOutput& out, double force) {
        KcWheel w;
        w.camber = out.geometry.camber / kDeg;
        w.toe = out.geometry.toe / kDeg;
        w.halfTrackChange = out.geometry.halfTrackChange * 1000.0;
        w.wheelbaseChange = out.geometry.wheelbaseChange * 1000.0;
        w.padForce = force;
        return w;
    };

    std::array<double, 2> restTravel{};
    for (int axle = 0; axle < 2; ++axle)
    {
        const CarCorner& left = car.corners[2 * axle];
        const CarCorner& right = car.corners[2 * axle + 1];
        const double halfTrack = 0.5 * std::abs(left.position.y - right.position.y);

        // Bounce: both wheels together from full rebound to full bump.
        {
            Walk walk{MakeAxleSuspension(car, axle)};
            settle(walk, -bounceRange, -bounceRange, 0.0);
            for (int s = 0; s <= bounceSteps; ++s)
            {
                const double z = -bounceRange + 2.0 * bounceRange * s / bounceSteps;
                const std::array<CornerOutput, 2> both = settle(walk, z, z, 0.0);
                const CornerOutput& lo = both[0];
                const CornerOutput& ro = both[1];
                KcBouncePoint p;
                p.travel = z * 1000.0;
                p.left = wheel(lo, PadForce(lo, 0.0));
                p.right = wheel(ro, PadForce(ro, 0.0));
                p.kingpinInclination = lo.geometry.kingpinInclination / kDeg;
                p.caster = lo.geometry.caster / kDeg;
                p.scrubRadius = lo.geometry.scrubRadius * 1000.0;
                p.casterTrail = lo.geometry.casterTrail * 1000.0;
                p.rollCenterHeight = lo.geometry.rollCenterHeight * 1000.0;
                p.contactPathAngle = lo.geometry.contactPathAngle / kDeg;
                p.centerPathAngle = lo.geometry.wheelCenterPathAngle / kDeg;
                result.bounce[axle].push_back(p);
            }
            std::vector<KcBouncePoint>& b = result.bounce[axle];
            for (std::size_t i = 0; i < b.size(); ++i)
            {
                const std::size_t i0 = i == 0 ? 0 : i - 1;
                const std::size_t i1 = i + 1 == b.size() ? i : i + 1;
                b[i].wheelRate = (b[i1].left.padForce - b[i0].left.padForce) / (b[i1].travel - b[i0].travel);
            }
            // Where the axle rests: the travel at which the pad carries the corner's static load (the design
            // position for a car balanced there, elsewhere when a rod length preloads the springs). The
            // summary is taken there, by the same central difference.
            double rest = 0.0;
            for (std::size_t i = 0; i + 1 < b.size(); ++i)
            {
                const double f0 = b[i].left.padForce - car.staticLoad[2 * axle];
                const double f1 = b[i + 1].left.padForce - car.staticLoad[2 * axle];
                if ((f0 <= 0.0 && f1 >= 0.0) || (f0 >= 0.0 && f1 <= 0.0))
                {
                    const double t = f1 != f0 ? f0 / (f0 - f1) : 0.0;
                    rest = (b[i].travel + t * (b[i + 1].travel - b[i].travel)) / 1000.0;
                    break;
                }
            }
            if (std::abs(rest) < 1e-9)
            {
                rest = 0.0;
            }
            restTravel[axle] = rest;
            const double step = 2.0 * bounceRange / bounceSteps;
            Walk at{MakeAxleSuspension(car, axle)};
            settle(at, rest - step, rest - step, 0.0);
            const std::array<CornerOutput, 2> below = settle(at, rest - step, rest - step, 0.0);
            const std::array<CornerOutput, 2> there = settle(at, rest, rest, 0.0);
            const std::array<CornerOutput, 2> above = settle(at, rest + step, rest + step, 0.0);
            KcBouncePoint lo;
            KcBouncePoint mid;
            KcBouncePoint hi;
            for (auto [point, outputs, z] : {std::tuple<KcBouncePoint*, const std::array<CornerOutput, 2>*, double>{&lo, &below, rest - step},
                                             {&mid, &there, rest},
                                             {&hi, &above, rest + step}})
            {
                const CornerOutput& o = (*outputs)[0];
                point->travel = z * 1000.0;
                point->left = wheel(o, PadForce(o, 0.0));
                point->kingpinInclination = o.geometry.kingpinInclination / kDeg;
                point->caster = o.geometry.caster / kDeg;
                point->scrubRadius = o.geometry.scrubRadius * 1000.0;
                point->casterTrail = o.geometry.casterTrail * 1000.0;
                point->rollCenterHeight = o.geometry.rollCenterHeight * 1000.0;
                point->contactPathAngle = o.geometry.contactPathAngle / kDeg;
                point->centerPathAngle = o.geometry.wheelCenterPathAngle / kDeg;
            }
            mid.wheelRate = (hi.left.padForce - lo.left.padForce) / (hi.travel - lo.travel);
            const double dz = (hi.travel - lo.travel) / 1000.0;
            KcAxleSummary& sum = result.axles[axle];
            sum.bumpSteer = (hi.left.toe - lo.left.toe) / dz;
            sum.camberGain = (hi.left.camber - lo.left.camber) / dz;
            sum.trackChange = (hi.left.halfTrackChange - lo.left.halfTrackChange) / dz;
            sum.wheelRate = mid.wheelRate;
            sum.rollCenterHeight = mid.rollCenterHeight;
            sum.kingpinInclination = mid.kingpinInclination;
            sum.caster = mid.caster;
            sum.scrubRadius = mid.scrubRadius;
            sum.casterTrail = mid.casterTrail;
            sum.contactPathAngle = mid.contactPathAngle;
            sum.centerPathAngle = mid.centerPathAngle;
        }

        // Roll: the body rolled to the right with the pads held, each wheel's travel the body's
        // drop at it; the anti-roll bar works against the travel difference.
        {
            Walk walk{MakeAxleSuspension(car, axle)};
            for (int s = 0; s <= 2 * rollSteps; ++s)
            {
                const double roll = (-rollRange + rollRange * s / rollSteps) * kDeg;
                const double zl = restTravel[axle] - left.position.y * std::sin(roll);
                const double zr = restTravel[axle] - right.position.y * std::sin(roll);
                const std::array<CornerOutput, 2> both = settle(walk, zl, zr, 0.0);
                const CornerOutput& lo = both[0];
                const CornerOutput& ro = both[1];
                const double arbLeft = -left.antiRollBarRate * (zl - zr);
                const double arbRight = -right.antiRollBarRate * (zr - zl);
                KcRollPoint p;
                p.roll = roll / kDeg;
                p.left = wheel(lo, PadForce(lo, arbLeft));
                p.right = wheel(ro, PadForce(ro, arbRight));
                // Camber to the road: the body's roll tips the left wheel's top inward, the right's out.
                p.left.camber -= p.roll;
                p.right.camber += p.roll;
                p.rollMoment = (p.right.padForce - p.left.padForce) * halfTrack;
                result.roll[axle].push_back(p);
            }
            const std::vector<KcRollPoint>& rl = result.roll[axle];
            const KcRollPoint& lo = rl[rollSteps - 1];
            const KcRollPoint& hi = rl[rollSteps + 1];
            const KcRollPoint& zero = rl[rollSteps];
            const double dphi = hi.roll - lo.roll;
            KcAxleSummary& sum = result.axles[axle];
            sum.rollStiffness = (hi.rollMoment - lo.rollMoment) / dphi;
            // Axle steer to the right (the outside of the left-hand turn this roll is): left toe-in,
            // right toe-out.
            const auto steerRight = [&](const KcRollPoint& p) {
                return 0.5 * ((p.left.toe - zero.left.toe) - (p.right.toe - zero.right.toe));
            };
            sum.rollSteer = (steerRight(hi) - steerRight(lo)) / dphi;
            sum.rollCamber = (hi.right.camber - lo.right.camber) / dphi;
        }
    }
    const double totalRoll = result.axles[0].rollStiffness + result.axles[1].rollStiffness;
    result.rollStiffnessFrontShare = totalRoll > 0.0 ? result.axles[0].rollStiffness / totalRoll : 0.0;

    // Steering: the rack across its travel with the wheels where they rest.
    {
        const CarCorner& left = car.corners[0];
        const CarCorner& right = car.corners[1];
        const double z = restTravel[0];
        Walk walk{MakeAxleSuspension(car, 0)};
        const std::array<CornerOutput, 2> straight = settle(walk, z, z, 0.0);
        const double toeL0 = straight[0].geometry.toe / kDeg;
        const double toeR0 = straight[1].geometry.toe / kDeg;
        const double lock = car.rackAtLock * rackFraction;
        const double track = std::abs(left.position.y - right.position.y);
        const int steps = 20;
        settle(walk, z, z, -lock);
        for (int s = 0; s <= 2 * steps; ++s)
        {
            const double rack = -lock + lock * s / steps;
            const std::array<CornerOutput, 2> both = settle(walk, z, z, rack);
            const CornerOutput& lo = both[0];
            const CornerOutput& ro = both[1];
            KcSteerPoint p;
            p.rack = rack * 1000.0;
            p.steeringWheel = car.rackAtLock != 0.0 ? rack / car.rackAtLock * car.steeringWheelLockDegrees : 0.0;
            p.left = lo.geometry.toe / kDeg - toeL0;
            p.right = -(ro.geometry.toe / kDeg - toeR0);
            const double outer = p.steeringWheel >= 0.0 ? p.left : -p.right;
            const double inner = p.steeringWheel >= 0.0 ? p.right : -p.left;
            if (outer > 1.0 && car.wheelbase > 0.0)
            {
                const double ideal = std::atan(1.0 / (1.0 / std::tan(outer * kDeg) - track / car.wheelbase)) / kDeg;
                p.ackermann = (inner - outer) / (ideal - outer) * 100.0;
            }
            result.steer.push_back(p);
        }
        const KcSteerPoint& a = result.steer[steps - 1];
        const KcSteerPoint& b = result.steer[steps + 1];
        const double meanTurn = 0.5 * ((b.left + b.right) - (a.left + a.right));
        result.steeringRatio = meanTurn != 0.0 ? (b.steeringWheel - a.steeringWheel) / meanTurn : 0.0;
    }

    // Anti-dive, anti-lift and anti-squat from the paths' angles in side view (the usual
    // tan(angle) * wheelbase / centre-of-mass height construction).
    if (car.cgHeight > 0.0)
    {
        const double lever = car.wheelbase / car.cgHeight;
        result.antiDiveFront = std::tan(result.axles[0].contactPathAngle * kDeg) * lever * car.frontBrakeShare * 100.0;
        result.antiLiftRear = -std::tan(result.axles[1].contactPathAngle * kDeg) * lever * (1.0 - car.frontBrakeShare) * 100.0;
        result.antiSquatRear = -std::tan(result.axles[1].centerPathAngle * kDeg) * lever * 100.0;
    }
    return result;
}

std::array<double, 4> RigModePattern(RigMode mode)
{
    switch (mode)
    {
    case RigMode::Heave:
        return {1.0, 1.0, 1.0, 1.0};
    case RigMode::Pitch:
        return {1.0, 1.0, -1.0, -1.0};
    case RigMode::Roll:
        return {1.0, -1.0, 1.0, -1.0};
    case RigMode::Warp:
        return {1.0, -1.0, -1.0, 1.0};
    }
    return {1.0, 1.0, 1.0, 1.0};
}

const char* RigModeName(RigMode mode)
{
    switch (mode)
    {
    case RigMode::Heave:
        return "heave";
    case RigMode::Pitch:
        return "pitch";
    case RigMode::Roll:
        return "roll";
    case RigMode::Warp:
        return "warp";
    }
    return "?";
}

SevenPostRig::SevenPostRig(const CarModel& car, bool friction, UnsprungScheme scheme)
    : m_car(car), m_scheme(scheme)
{
    for (int i = 0; i < 4; ++i)
    {
        CarCorner corner = m_car.corners[i];
        corner.hasFriction = corner.hasFriction && friction;
        if (corner.hubMass <= 0.0 || corner.tyreRate <= 0.0)
        {
            throw std::invalid_argument("SevenPostRig needs each corner's hub mass and tyre rate");
        }
        m_staticTyreLoad[i] = m_car.staticLoad[i] + corner.hubMass * kGravity;
        m_tyreLoad[i] = m_staticTyreLoad[i];
    }
    for (int axle = 0; axle < 2; ++axle)
    {
        m_axles[axle] = MakeAxleSuspension(m_car, axle, friction);
    }
    // A car whose springs do not carry it at the design position (a rod length preloads them instead)
    // settles where it rests before the rig starts: as long as the body still moves, up to 5 s.
    const std::array<double, 4> zero{};
    Step(zero, zero, 0.0, 0.0, 0.0, 1e-3);
    if (std::abs(m_heaveAccel) > 1e-3 || std::abs(m_pitchAccel) > 1e-3 || std::abs(m_rollAccel) > 1e-3)
    {
        for (int step = 0; step < 5000; ++step)
        {
            Step(zero, zero, 0.0, 0.0, 0.0, 1e-3);
            if (step > 500 && std::abs(m_heaveRate) < 1e-5 && std::abs(m_pitchRate) < 1e-5 && std::abs(m_rollRate) < 1e-5)
            {
                break;
            }
        }
        m_restHeave = m_heave;
        m_restPitch = m_pitch;
        m_restRoll = m_roll;
    }
}

void SevenPostRig::Step(const std::array<double, 4>& pads, const std::array<double, 4>& padRates, double heaveForce, double pitchMoment, double rollMoment, double dt)
{
    std::array<double, 4> travel{};
    std::array<double, 4> travelRate{};
    for (int i = 0; i < 4; ++i)
    {
        const Vec3& p = m_car.corners[i].position;
        const double body = m_heave + p.x * m_pitch + p.y * m_roll;
        const double bodyRate = m_heaveRate + p.x * m_pitchRate + p.y * m_rollRate;
        travel[i] = m_wheel[i] - body;
        travelRate[i] = m_wheelRate[i] - bodyRate;
    }
    double heave = heaveForce - m_car.sprungMass * kGravity;
    double pitch = pitchMoment;
    double roll = rollMoment;
    std::array<double, 4> wheelAccel{};
    for (int axle = 0; axle < 2; ++axle)
    {
        std::array<CornerInput, 2> in{};
        for (int side = 0; side < 2; ++side)
        {
            in[side].travel = travel[2 * axle + side];
            in[side].travelRate = travelRate[2 * axle + side];
            in[side].dt = dt;
        }
        m_axles[axle]->Step(in);
    }
    if (m_scheme == UnsprungScheme::GameLinearlyImplicit)
    {
        StepGameScheme(pads, padRates, heaveForce, pitchMoment, rollMoment, dt, travel, travelRate);
        return;
    }
    for (int i = 0; i < 4; ++i)
    {
        const CarCorner& corner = m_car.corners[i];
        const CornerOutput& out = m_axles[i / 2]->Output(i % 2);
        const int other = i ^ 1;
        const double arb = -corner.antiRollBarRate * (travel[i] - travel[other]);
        // Generalised force on the travel (vertical, between hub and body): pushes the wheel down
        // and the body up when negative.
        const double g = out.strutTravelForce + arb;
        const double deflection = m_staticTyreLoad[i] / corner.tyreRate + pads[i] - m_wheel[i];
        double tyre = corner.tyreRate * deflection + corner.tyreDamping * (padRates[i] - m_wheelRate[i]);
        tyre = deflection > 0.0 ? std::max(tyre, 0.0) : 0.0;
        m_tyreLoad[i] = tyre;
        wheelAccel[i] = (tyre + g - corner.hubMass * kGravity) / corner.hubMass;
        const double onBody = -g;
        heave += onBody;
        pitch += onBody * corner.position.x;
        roll += onBody * corner.position.y;
        m_travel[i] = travel[i];
    }
    m_heaveAccel = heave / m_car.sprungMass;
    m_pitchAccel = pitch / m_car.pitchInertia;
    m_rollAccel = roll / m_car.rollInertia;
    m_heaveRate += m_heaveAccel * dt;
    m_pitchRate += m_pitchAccel * dt;
    m_rollRate += m_rollAccel * dt;
    m_heave += m_heaveRate * dt;
    m_pitch += m_pitchRate * dt;
    m_roll += m_rollRate * dt;
    for (int i = 0; i < 4; ++i)
    {
        m_wheelRate[i] += wheelAccel[i] * dt;
        m_wheel[i] += m_wheelRate[i] * dt;
    }
}

void SevenPostRig::StepGameScheme(const std::array<double, 4>& pads, const std::array<double, 4>& padRates, double heaveForce, double pitchMoment, double rollMoment,
                                  double dt, const std::array<double, 4>& travel, const std::array<double, 4>& travelRate)
{
    // The body as the physics engine has it: sprung mass and hubs as one rigid body, the hubs (and
    // their weight) at their corners. In the rig's coordinates (heave, pitch, roll about the sprung
    // mass's centre) its mass matrix is the sprung mass's plus each hub's J^T m J, J = (1, x, y).
    Mat3 massMatrix(0.0);
    massMatrix[0][0] = m_car.sprungMass;
    massMatrix[1][1] = m_car.pitchInertia;
    massMatrix[2][2] = m_car.rollInertia;
    for (const CarCorner& corner : m_car.corners)
    {
        const Vec3 j(1.0, corner.position.x, corner.position.y);
        massMatrix += corner.hubMass * glm::outerProduct(j, j);
    }
    Vec3 generalised(heaveForce - m_car.sprungMass * kGravity, pitchMoment, rollMoment);
    std::array<double, 4> nextTravelRate{};
    for (int i = 0; i < 4; ++i)
    {
        const CarCorner& corner = m_car.corners[i];
        const CornerOutput& out = m_axles[i / 2]->Output(i % 2);
        const double z = travel[i];
        const double v = travelRate[i];
        const int other = i ^ 1;
        const double arb = -corner.antiRollBarRate * (travel[i] - travel[other]);

        // The tyre at the step's start, with its slopes against the travel (the body held) while pressed.
        const double deflection = m_staticTyreLoad[i] / corner.tyreRate + pads[i] - m_wheel[i];
        double tyre = 0.0;
        double tyreSlope = 0.0;
        double tyreRateSlope = 0.0;
        if (deflection > 0.0)
        {
            tyre = corner.tyreRate * deflection + corner.tyreDamping * (padRates[i] - m_wheelRate[i]);
            if (tyre > 0.0)
            {
                tyreSlope = -corner.tyreRate;
                tyreRateSlope = -corner.tyreDamping;
            }
            else
            {
                tyre = 0.0;
            }
        }

        // The body's specific force where the hub rides, from the last step's acceleration.
        const Vec3& p = corner.position;
        const double bodyAccel = m_heaveAccel + p.x * m_pitchAccel + p.y * m_rollAccel;
        const double inertia = -corner.hubMass * (bodyAccel + kGravity);

        const double force = tyre + out.strutTravelForce + arb + inertia;
        const double stiffness = out.strutTravelStiffness - corner.antiRollBarRate + tyreSlope;
        const double damping = out.strutTravelDamping + tyreRateSlope;
        const double change = dt * (force + 0.5 * dt * stiffness * v) / (corner.hubMass - 0.5 * dt * damping - 0.25 * dt * dt * stiffness);
        const double rate = v + change;
        const double moved = dt * (v + 0.5 * change);
        const double travelAccel = change / dt;
        const double applied = std::max(tyre + tyreSlope * moved + tyreRateSlope * change, 0.0);
        m_tyreLoad[i] = applied;
        m_travel[i] = z + moved;
        nextTravelRate[i] = rate;

        const double onBody = applied - corner.hubMass * (travelAccel + kGravity);
        generalised += onBody * Vec3(1.0, p.x, p.y);
    }
    const Vec3 accel = glm::inverse(massMatrix) * generalised;
    m_heaveAccel = accel.x;
    m_pitchAccel = accel.y;
    m_rollAccel = accel.z;
    m_heaveRate += m_heaveAccel * dt;
    m_pitchRate += m_pitchAccel * dt;
    m_rollRate += m_rollAccel * dt;
    m_heave += m_heaveRate * dt;
    m_pitch += m_pitchRate * dt;
    m_roll += m_rollRate * dt;
    for (int i = 0; i < 4; ++i)
    {
        const Vec3& p = m_car.corners[i].position;
        m_wheel[i] = m_heave + p.x * m_pitch + p.y * m_roll + m_travel[i];
        m_wheelRate[i] = m_heaveRate + p.x * m_pitchRate + p.y * m_rollRate + nextTravelRate[i];
    }
}

SineSweep::SineSweep(double startHz, double endHz, int cycleCount)
    : f0(startHz), fE(endHz), cycles(cycleCount)
{
    // Rill eq. 6.61: q = ln(fE / f0) / N, p = q / (f0 (1 - (f0/fE)^(1/N))).
    q = std::log(fE / f0) / cycles;
    p = q / (f0 * (1.0 - std::pow(f0 / fE, 1.0 / cycles)));
}

double SineSweep::CycleStart(int n) const
{
    return p / q * (1.0 - std::exp(-n * q));
}

double SineSweep::Duration() const
{
    return CycleStart(cycles + 1);
}

double SineSweep::Phase(double t) const
{
    return 2.0 * kPi / q * std::log(p / (p - q * t));
}

double SineSweep::Frequency(double t) const
{
    return 1.0 / (p - q * t);
}

SweepResult RunSweep(const CarModel& car, RigMode mode, const SineSweep& sweep, double amplitude, double maxVelocity, bool friction, double dt, UnsprungScheme scheme)
{
    SevenPostRig rig(car, friction, scheme);
    SweepResult result;
    result.mode = mode;
    const std::array<double, 4> pattern = RigModePattern(mode);
    std::array<double, 4> zero{};
    for (int i = 0; i < 500; ++i)
    {
        rig.Step(zero, zero, 0.0, 0.0, 0.0, dt);
    }

    std::array<double, 4> pads{};
    std::array<double, 4> padRates{};
    double previousPad = 0.0;
    int cycle = 0;
    double cycleEnd = sweep.CycleStart(1);
    std::vector<double> phases, pad, body, bodyAccel;
    std::array<std::vector<double>, 4> load, wheel;
    const double duration = sweep.Duration();
    for (double t = 0.0; t < duration && cycle <= sweep.cycles; t += dt)
    {
        const double frequency = sweep.Frequency(t);
        const double a = std::min(amplitude, maxVelocity / (2.0 * kPi * frequency));
        const double phase = sweep.Phase(t);
        const double x = a * std::sin(phase);
        const double rate = (x - previousPad) / dt;
        previousPad = x;
        for (int i = 0; i < 4; ++i)
        {
            pads[i] = pattern[i] * x;
            padRates[i] = pattern[i] * rate;
        }
        rig.Step(pads, padRates, 0.0, 0.0, 0.0, dt);
        phases.push_back(phase);
        pad.push_back(x);
        body.push_back(ModeBody(rig, mode));
        bodyAccel.push_back(ModeBodyAccel(rig, mode));
        for (int i = 0; i < 4; ++i)
        {
            load[i].push_back(rig.TyreLoad(i));
            wheel[i].push_back(rig.WheelHeight(i) * pattern[i]);
        }
        if (t + dt >= cycleEnd)
        {
            const double start = sweep.CycleStart(cycle);
            SweepCycle c;
            c.frequency = 1.0 / (cycleEnd - start);
            const Harmonic h = FirstHarmonic(pad, phases);
            c.padAmplitude = h.Amplitude();
            if (c.padAmplitude > 0.0)
            {
                const Harmonic hb = FirstHarmonic(body, phases);
                c.bodyGain = hb.Amplitude() / c.padAmplitude;
                c.bodyPhase = WrapDegrees((hb.Phase() - h.Phase()) / kDeg);
                const double padAccel = c.padAmplitude * std::pow(2.0 * kPi * c.frequency, 2.0);
                c.bodyAccelGain = FirstHarmonic(bodyAccel, phases).Amplitude() / padAccel;
                for (int i = 0; i < 4; ++i)
                {
                    const double fixed = rig.StaticTyreLoad(i);
                    c.loadVariation[i] = FirstHarmonic(load[i], phases).Amplitude() / fixed;
                    c.wheelGain[i] = FirstHarmonic(wheel[i], phases).Amplitude() / c.padAmplitude;
                    double square = 0.0;
                    for (double v : load[i])
                    {
                        square += (v - fixed) * (v - fixed);
                    }
                    c.loadRms[i] = std::sqrt(square / static_cast<double>(load[i].size())) / fixed;
                }
            }
            result.cycles.push_back(c);
            phases.clear();
            pad.clear();
            body.clear();
            bodyAccel.clear();
            for (int i = 0; i < 4; ++i)
            {
                load[i].clear();
                wheel[i].clear();
            }
            ++cycle;
            cycleEnd = sweep.CycleStart(cycle + 1);
        }
    }

    // The body's resonance, its half-power damping, the wheels' hop and the worst load variation.
    std::size_t peak = 0;
    for (std::size_t i = 0; i < result.cycles.size(); ++i)
    {
        if (result.cycles[i].bodyGain > result.cycles[peak].bodyGain)
        {
            peak = i;
        }
        for (double v : result.cycles[i].loadVariation)
        {
            result.peakLoadVariation = std::max(result.peakLoadVariation, v);
        }
    }
    if (!result.cycles.empty() && mode != RigMode::Warp)
    {
        result.bodyFrequency = result.cycles[peak].frequency;
        result.bodyPeakGain = result.cycles[peak].bodyGain;
        const double half = result.bodyPeakGain / std::sqrt(2.0);
        double lower = 0.0;
        double upper = 0.0;
        for (std::size_t i = peak; i-- > 0;)
        {
            if (result.cycles[i].bodyGain < half)
            {
                lower = result.cycles[i].frequency;
                break;
            }
        }
        for (std::size_t i = peak; i < result.cycles.size(); ++i)
        {
            if (result.cycles[i].bodyGain < half)
            {
                upper = result.cycles[i].frequency;
                break;
            }
        }
        if (lower > 0.0 && upper > 0.0)
        {
            result.bodyDamping = (upper - lower) / (2.0 * result.bodyFrequency);
        }
    }
    // The wheels' hop: the highest local peak of their gain above 6 Hz (none when the dampers hold
    // the hubs past critical, as a race car's low-speed damping can).
    double hop = 0.0;
    const auto wheelGain = [&](std::size_t i) {
        const SweepCycle& c = result.cycles[i];
        return 0.25 * (c.wheelGain[0] + c.wheelGain[1] + c.wheelGain[2] + c.wheelGain[3]);
    };
    for (std::size_t i = 1; i + 1 < result.cycles.size(); ++i)
    {
        const double gain = wheelGain(i);
        if (result.cycles[i].frequency > 6.0 && gain > wheelGain(i - 1) && gain >= wheelGain(i + 1) && gain > hop)
        {
            hop = gain;
            result.wheelHopFrequency = result.cycles[i].frequency;
        }
    }
    return result;
}

StepResponse RunStep(const CarModel& car, RigMode mode, double height, double seconds, bool friction, double dt, UnsprungScheme scheme)
{
    SevenPostRig rig(car, friction, scheme);
    const std::array<double, 4> pattern = RigModePattern(mode);
    StepResponse r;
    std::array<double, 4> pads{};
    std::array<double, 4> rates{};
    const double ramp = 0.005;
    for (double t = 0.0; t < seconds; t += dt)
    {
        const double x = height * std::clamp(t / ramp, 0.0, 1.0);
        const double rate = t < ramp ? height / ramp : 0.0;
        for (int i = 0; i < 4; ++i)
        {
            pads[i] = pattern[i] * x;
            rates[i] = pattern[i] * rate;
        }
        rig.Step(pads, rates, 0.0, 0.0, 0.0, dt);
        r.time.push_back(t);
        r.body.push_back(ModeBody(rig, mode));
        r.frontLoad.push_back(rig.TyreLoad(0));
    }
    const double final = r.body.back();
    if (std::abs(final) < 1e-12)
    {
        return r;
    }
    double extreme = 0.0;
    for (double v : r.body)
    {
        extreme = std::max(extreme, v / final);
    }
    r.overshoot = std::max(extreme - 1.0, 0.0);
    // Successive extremes of the error: the logarithmic decrement over a full period.
    std::vector<double> peaks;
    for (std::size_t i = 1; i + 1 < r.body.size(); ++i)
    {
        const double e0 = r.body[i - 1] - final;
        const double e1 = r.body[i] - final;
        const double e2 = r.body[i + 1] - final;
        if ((e1 > e0 && e1 >= e2) || (e1 < e0 && e1 <= e2))
        {
            if (std::abs(e1) > 0.005 * std::abs(final))
            {
                peaks.push_back(std::abs(e1));
            }
        }
    }
    if (r.overshoot > 0.005 && peaks.size() >= 3)
    {
        const double delta = std::log(peaks[0] / peaks[2]);
        r.dampingRatio = delta / std::sqrt(4.0 * kPi * kPi + delta * delta);
    }
    else
    {
        r.dampingRatio = 1.0;
    }
    for (std::size_t i = r.body.size(); i-- > 0;)
    {
        if (std::abs(r.body[i] - final) > 0.02 * std::abs(final))
        {
            r.settlingTime = r.time[i];
            break;
        }
    }
    return r;
}

std::vector<AeroPoint> RunAeroLoads(const CarModel& car, double maxDownforce, double frontShare, int steps, bool friction)
{
    SevenPostRig rig(car, friction);
    const double dt = 1e-3;
    const std::array<double, 4> zero{};
    const double frontX = 0.5 * (car.corners[0].position.x + car.corners[1].position.x);
    const double rearX = 0.5 * (car.corners[2].position.x + car.corners[3].position.x);
    std::vector<AeroPoint> points;
    for (int s = 0; s <= steps; ++s)
    {
        const double downforce = maxDownforce * s / steps;
        const double heave = -downforce;
        const double pitch = -(frontShare * downforce * frontX + (1.0 - frontShare) * downforce * rearX);
        double h = 0.0;
        double p = 0.0;
        int samples = 0;
        for (int i = 0; i < 1500; ++i)
        {
            rig.Step(zero, zero, heave, pitch, 0.0, dt);
            if (i >= 1200)
            {
                h += rig.Heave();
                p += rig.Pitch();
                ++samples;
            }
        }
        h /= samples;
        p /= samples;
        AeroPoint a;
        a.downforce = downforce;
        a.frontHeight = (h + frontX * p) * 1000.0;
        a.rearHeight = (h + rearX * p) * 1000.0;
        points.push_back(a);
    }
    return points;
}

WarpResult RunWarp(const CarModel& car, double padWarp, bool friction)
{
    const auto diagonal = [&](double warp) {
        SevenPostRig rig(car, friction);
        const std::array<double, 4> pattern = RigModePattern(RigMode::Warp);
        std::array<double, 4> pads{};
        const std::array<double, 4> rates{};
        double sum = 0.0;
        int samples = 0;
        for (int i = 0; i < 2500; ++i)
        {
            const double ramp = std::min(i / 500.0, 1.0);
            for (int c = 0; c < 4; ++c)
            {
                pads[c] = pattern[c] * warp * ramp;
            }
            rig.Step(pads, rates, 0.0, 0.0, 0.0, 1e-3);
            if (i >= 2000)
            {
                sum += 0.5 * (rig.TyreLoad(0) + rig.TyreLoad(3) - rig.TyreLoad(1) - rig.TyreLoad(2));
                ++samples;
            }
        }
        return sum / samples;
    };
    WarpResult r;
    r.padWarp = padWarp;
    r.diagonalTransfer = diagonal(padWarp) - diagonal(0.0);
    r.warpStiffness = r.diagonalTransfer / (padWarp * 1000.0);
    return r;
}

RandomRoad::RandomRoad(double phi0, double waviness, std::uint32_t seed)
{
    // Rill's random road as a sum of sines: Omega from 2 pi / 200 m to 2 pi / 0.2 m.
    const int n = 400;
    const double omegaMin = 2.0 * kPi / 200.0;
    const double omegaMax = 2.0 * kPi / 0.2;
    const double dOmega = (omegaMax - omegaMin) / (n - 1);
    m_omega.resize(n);
    m_amplitude.resize(n);
    m_phase[0].resize(n);
    m_phase[1].resize(n);
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> uniform(0.0, 2.0 * kPi);
    for (int k = 0; k < n; ++k)
    {
        m_omega[k] = omegaMin + k * dOmega;
        m_amplitude[k] = std::sqrt(2.0 * phi0 * std::pow(m_omega[k], -waviness) * dOmega);
        m_phase[0][k] = uniform(random);
        m_phase[1][k] = uniform(random);
    }
}

double RandomRoad::Height(int track, double distance) const
{
    double z = 0.0;
    for (size_t k = 0; k < m_omega.size(); ++k)
    {
        z += m_amplitude[k] * std::sin(m_omega[k] * distance + m_phase[track][k]);
    }
    return z;
}

RoadResult RunRoad(const CarModel& car, double speed, double phi0, double waviness, double seconds, std::uint32_t seed, bool friction, double dt, UnsprungScheme scheme)
{
    const RandomRoad randomRoad(phi0, waviness, seed);
    const auto road = [&](int track, double s) {
        return randomRoad.Height(track, s);
    };
    const double frontX = 0.5 * (car.corners[0].position.x + car.corners[1].position.x);
    const double rearX = 0.5 * (car.corners[2].position.x + car.corners[3].position.x);
    const double base0 = road(0, 0.0);
    const double base1 = road(1, 0.0);
    const double baseRear0 = road(0, -(frontX - rearX));
    const double baseRear1 = road(1, -(frontX - rearX));

    SevenPostRig rig(car, friction, scheme);
    RoadResult r;
    std::array<double, 4> pads{};
    std::array<double, 4> previous{};
    std::array<double, 4> rates{};
    std::array<double, 4> square{};
    double accel = 0.0;
    double travel = 0.0;
    int samples = 0;
    // Fade the road in over the first second so the rig starts at rest.
    for (double t = 0.0; t < seconds; t += dt)
    {
        const double s = speed * t;
        const double fade = std::min(t, 1.0);
        pads[0] = fade * (road(0, s) - base0);
        pads[1] = fade * (road(1, s) - base1);
        pads[2] = fade * (road(0, s - (frontX - rearX)) - baseRear0);
        pads[3] = fade * (road(1, s - (frontX - rearX)) - baseRear1);
        for (int i = 0; i < 4; ++i)
        {
            rates[i] = t > 0.0 ? (pads[i] - previous[i]) / dt : 0.0;
            previous[i] = pads[i];
        }
        rig.Step(pads, rates, 0.0, 0.0, 0.0, dt);
        if (t >= 2.0)
        {
            bool off = false;
            for (int i = 0; i < 4; ++i)
            {
                const double d = (rig.TyreLoad(i) - rig.StaticTyreLoad(i)) / rig.StaticTyreLoad(i);
                square[i] += d * d;
                off = off || rig.TyreLoad(i) <= 0.0;
            }
            accel += rig.HeaveAcceleration() * rig.HeaveAcceleration();
            travel += rig.Travel(0) * rig.Travel(0);
            r.liftOffSeconds += off ? dt : 0.0;
            ++samples;
        }
    }
    if (samples == 0)
    {
        throw std::invalid_argument("RunRoad: the road must run longer than its 2 s lead-in");
    }
    for (int i = 0; i < 4; ++i)
    {
        r.loadRms[i] = std::sqrt(square[i] / samples);
    }
    r.bodyAccelRms = std::sqrt(accel / samples);
    r.travelRms = std::sqrt(travel / samples);
    return r;
}
}
