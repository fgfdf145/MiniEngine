#include "vehicle_suspension.h"

#include <engine/suspension/suspension_kinematics.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace me
{

namespace
{
suspension::Vec3 ToSuspension(const glm::vec3& v)
{
    return suspension::Vec3(v.x, v.y, v.z);
}

// The axle's linkage for the left wheel, with the static camber in its wheel axis.
suspension::SuspensionDefinition LeftDefinition(const VehicleSuspensionAxle& axle, double tyreRadius, bool steered)
{
    const double camber = axle.staticCamberDegrees * std::numbers::pi / 180.0;
    // Camber positive with the top outward: the axle's outer end then dips.
    const suspension::Vec3 wheelAxis(0.0, std::cos(camber), -std::sin(camber));
    suspension::SuspensionDefinition def;
    if (axle.type == VehicleSuspensionType::DoubleWishbone)
    {
        suspension::DoubleWishboneHardpoints hp;
        hp.lowerFront = ToSuspension(axle.lowerFront);
        hp.lowerRear = ToSuspension(axle.lowerRear);
        hp.lowerBall = ToSuspension(axle.lowerBall);
        hp.upperFront = ToSuspension(axle.upperFront);
        hp.upperRear = ToSuspension(axle.upperRear);
        hp.upperBall = ToSuspension(axle.upperBall);
        hp.tieInner = ToSuspension(axle.tieInner);
        hp.tieOuter = ToSuspension(axle.tieOuter);
        hp.wheelCenter = suspension::Vec3(0.0);
        // The data gives the springs as wheel rates: the seat is only for the motion ratio output.
        hp.springLower = hp.lowerBall + 0.5 * (0.5 * (hp.lowerFront + hp.lowerRear) - hp.lowerBall) + suspension::Vec3(0.0, 0.0, 0.01);
        hp.springUpper = hp.springLower + suspension::Vec3(0.0, -0.05, 0.4);
        hp.steered = steered;
        def = suspension::MakeDoubleWishbone(hp, tyreRadius, wheelAxis);
    }
    else if (axle.type == VehicleSuspensionType::MacPherson)
    {
        suspension::MacPhersonHardpoints hp;
        hp.lowerFront = ToSuspension(axle.lowerFront);
        hp.lowerRear = ToSuspension(axle.lowerRear);
        hp.lowerBall = ToSuspension(axle.lowerBall);
        hp.strutTop = ToSuspension(axle.strutTop);
        hp.strutLower = ToSuspension(axle.strutLower);
        hp.tieInner = ToSuspension(axle.tieInner);
        hp.tieOuter = ToSuspension(axle.tieOuter);
        hp.wheelCenter = suspension::Vec3(0.0);
        hp.steered = steered;
        def = suspension::MakeMacPherson(hp, tyreRadius, wheelAxis);
    }
    else
    {
        throw std::invalid_argument("BuildVehicleCorner: the axle has no suspension type");
    }
    def.vehicleCenter = suspension::Vec3(0.0, -0.5 * std::max(static_cast<double>(axle.track), 0.5), 0.0);
    return def;
}

double ToeAt(const suspension::SuspensionDefinition& def)
{
    suspension::Kinematics kinematics(suspension::Compile(def));
    suspension::KinematicOutputs out;
    suspension::ComputeOutputs(kinematics, out);
    return out.toe;
}

// Assetto Corsa's TOE_OUT changes the tie rod's length; which way gives toe-out depends on whether
// the rod is ahead of the axle or behind it, so try one and keep the sign that turns the front out.
void ApplyToeOut(suspension::SuspensionDefinition& def, double toeOutLength)
{
    if (toeOutLength == 0.0)
    {
        return;
    }
    const double neutral = ToeAt(def);
    suspension::SuspensionDefinition longer = def;
    longer.lengthAdjusts.push_back({"tie_inner", "tie_outer", std::abs(toeOutLength)});
    const bool longerTurnsOut = ToeAt(longer) < neutral;
    const bool wantOut = toeOutLength > 0.0;
    def.lengthAdjusts.push_back({"tie_inner", "tie_outer", (longerTurnsOut == wantOut ? 1.0 : -1.0) * std::abs(toeOutLength)});
}
}

VehicleCornerSetup BuildVehicleCorner(const VehicleSettings& settings, size_t wheelIndex, double staticLoad)
{
    const bool front = wheelIndex < 2;
    const bool left = wheelIndex % 2 == 0;
    const VehicleSuspensionAxle& axle = front ? settings.frontSuspension : settings.rearSuspension;
    const VehicleWheelGeometry mount = GetVehicleWheelMount(settings, wheelIndex);

    VehicleCornerSetup setup;
    setup.front = front;
    setup.left = left;
    // A solid axle's wheels have no linkage of their own (BuildSolidAxle has it), only their units.
    if (axle.type != VehicleSuspensionType::SolidAxle)
    {
        setup.definition = LeftDefinition(axle, std::max(mount.radius, 0.05f), front);
        ApplyToeOut(setup.definition, axle.toeOutRodLength);
        if (!left)
        {
            setup.definition = suspension::MirrorToRight(setup.definition);
        }
    }

    // Springs at the wheel: preload for the static load, the rate rising by the progressive rate.
    suspension::StrutUnitSettings& unit = setup.unit;
    unit.springPreload = staticLoad;
    unit.springPushesOnly = true;
    unit.coilSpring = suspension::Curve::Polynomial(std::max(axle.wheelRate, 1.0f), 0.5 * axle.progressiveRate, 0.0);
    if (axle.bumpStopRate > 0.0f && axle.bumpStopTravel > 0.0f)
    {
        unit.bumpStop = suspension::Curve::Stop(axle.bumpStopTravel, axle.bumpStopRate, 0.0, 1);
    }
    // The damper: slow and fast rates each way, the fast one past its threshold (velocity positive
    // in compression).
    // Without a fast stage the slow rate runs on (a knee at 1 m/s with the same slope).
    const bool fastBumpStage = axle.dampFastBump > 0.0f && axle.dampFastBumpThreshold > 0.0f;
    const bool fastReboundStage = axle.dampFastRebound > 0.0f && axle.dampFastReboundThreshold > 0.0f;
    const double bumpKnee = fastBumpStage ? axle.dampFastBumpThreshold : 1.0;
    const double reboundKnee = fastReboundStage ? axle.dampFastReboundThreshold : 1.0;
    const double fastBump = fastBumpStage ? axle.dampFastBump : axle.dampBump;
    const double fastRebound = fastReboundStage ? axle.dampFastRebound : axle.dampRebound;
    const double bumpKneeForce = axle.dampBump * bumpKnee;
    const double reboundKneeForce = axle.dampRebound * reboundKnee;
    unit.damper = suspension::Curve::Polyline(
        {-reboundKnee - 1.0, -reboundKnee, 0.0, bumpKnee, bumpKnee + 1.0},
        {-reboundKneeForce - fastRebound, -reboundKneeForce, 0.0, bumpKneeForce, bumpKneeForce + fastBump});

    if (axle.frictionCoulomb > 0.0f || axle.frictionBreakaway > 0.0f)
    {
        setup.hasFriction = true;
        suspension::LuGreParameters& f = setup.friction;
        f.stribeck.coulomb = {axle.frictionCoulomb, 0.0, 0.0};
        f.stribeck.breakaway = {std::max(axle.frictionBreakaway, axle.frictionCoulomb), 0.0, 0.0};
        f.stribeck.stribeckVelocity = 0.005;
        // Bristles that reach the breakaway force within about a tenth of a millimetre.
        f.bristleStiffness = std::max(static_cast<double>(axle.frictionBreakaway), 1.0) / 1e-4;
        f.bristleDamping = 2.0 * std::sqrt(f.bristleStiffness * 10.0);
    }
    setup.antiRollBarRate = axle.antiRollBarRate;
    setup.hubMass = axle.hubMass;
    setup.bumpTravel = std::max(static_cast<double>(axle.bumpStopTravel), 0.03) + 0.04;
    setup.droopTravel = axle.reboundStopTravel > 0.0f ? axle.reboundStopTravel : 0.08;
    return setup;
}

suspension::StrutUnit MakeVehicleCornerUnit(const VehicleCornerSetup& setup)
{
    if (setup.hasFriction)
    {
        return suspension::StrutUnit(setup.unit, std::make_unique<suspension::LuGreFriction>(setup.friction));
    }
    return suspension::StrutUnit(setup.unit, std::make_unique<suspension::NoFriction>());
}

suspension::SolidAxleDefinition BuildSolidAxle(const VehicleSuspensionAxle& axle, double tyreRadius)
{
    suspension::SolidAxleDefinition def;
    for (const VehicleAxleLink& link : axle.axleLinks)
    {
        def.links.push_back({ToSuspension(link.chassis), ToSuspension(link.axle)});
    }
    def.track = std::max(static_cast<double>(axle.track), 0.5);
    def.tyreRadius = std::max(tyreRadius, 0.05);
    def.springPosition = std::clamp(static_cast<double>(axle.axleSpringPosition), 0.0, 1.0);
    def.lateralStiffness = std::max(static_cast<double>(axle.axleLateralStiffness), 0.0);
    return def;
}

double FitSteeringRackTravel(const VehicleSettings& settings)
{
    if (settings.frontSuspension.type == VehicleSuspensionType::SolidAxle)
    {
        return 0.0; // a solid front axle does not steer here
    }
    const VehicleCornerSetup setup = BuildVehicleCorner(settings, 0, 1.0);
    const auto toeAt = [&](double rack, double& reached) {
        suspension::Kinematics kinematics(suspension::Compile(setup.definition));
        const int steps = 40;
        for (int i = 1; i <= steps; ++i)
        {
            kinematics.Solve(0.0, rack * i / steps);
        }
        reached = kinematics.Rack();
        suspension::KinematicOutputs out;
        suspension::ComputeOutputs(kinematics, out);
        return out.toe;
    };
    double reached = 0.0;
    const double neutral = toeAt(0.0, reached);
    // The rack's sign that steers right: the left wheel toes in.
    const double probe = toeAt(0.005, reached) - neutral;
    const double sign = probe >= 0.0 ? 1.0 : -1.0;
    if (settings.steeringRackTravel > 0.0f)
    {
        return sign * settings.steeringRackTravel;
    }
    const double target = std::clamp(settings.maxSteerAngleDegrees, 1.0f, 60.0f) * std::numbers::pi / 180.0;
    // The left wheel is the outer one steering right and the inner one steering left.
    const auto meanAngle = [&](double travel) {
        double a = 0.0;
        double b = 0.0;
        const double outer = std::abs(toeAt(sign * travel, a) - neutral);
        const double inner = std::abs(toeAt(-sign * travel, b) - neutral);
        return 0.5 * (outer + inner);
    };
    double low = 0.0;
    double high = 0.02;
    while (meanAngle(high) < target && high < 0.3)
    {
        low = high;
        high *= 1.5;
    }
    for (int i = 0; i < 40; ++i)
    {
        const double mid = 0.5 * (low + high);
        (meanAngle(mid) < target ? low : high) = mid;
    }
    return sign * 0.5 * (low + high);
}

suspension::CarModel BuildCarModel(const VehicleCarSpec& dryspec, const std::string& name)
{
    const VehicleCarSpec spec = WithStartingFuel(dryspec);
    if (!spec.frontSuspension.has_value() || !spec.rearSuspension.has_value() || !spec.massKg.has_value() || !spec.wheelbase.has_value() || !spec.frontWeightShare.has_value())
    {
        throw std::invalid_argument("BuildCarModel: the car's data lacks its linkage, mass, wheelbase or weight split");
    }
    VehicleSettings settings = ApplyCarSpec(VehicleSettings{}, spec);
    settings.wheelRadius = spec.frontSuspension->tyreRadius > 0.0f ? spec.frontSuspension->tyreRadius : settings.wheelRadius;

    suspension::CarModel car;
    car.name = name;
    car.mass = *spec.massKg;
    car.wheelbase = *spec.wheelbase;
    car.frontBrakeShare = spec.frontBrakeShare.value_or(0.6f);
    for (size_t index = 0; index < 4; ++index)
    {
        const bool front = index < 2;
        const VehicleSuspensionAxle& axle = front ? *spec.frontSuspension : *spec.rearSuspension;
        const VehicleCornerSetup setup = BuildVehicleCorner(settings, index, 0.0);
        suspension::CarCorner& corner = car.corners[index];
        corner.definition = setup.definition;
        corner.unit = setup.unit;
        corner.hasFriction = setup.hasFriction;
        corner.friction = setup.friction;
        corner.antiRollBarRate = setup.antiRollBarRate;
        corner.hubMass = axle.hubMass;
        corner.tyreRate = axle.tyreRate;
        corner.tyreDamping = axle.tyreDamping;
        const double halfTrack = 0.5 * axle.track;
        corner.position = suspension::Vec3(front ? car.wheelbase : 0.0, index % 2 == 0 ? halfTrack : -halfTrack, 0.0);
        if (axle.type == VehicleSuspensionType::SolidAxle && index % 2 == 0)
        {
            car.solidAxles[index / 2] = BuildSolidAxle(axle, axle.tyreRadius > 0.0f ? axle.tyreRadius : settings.wheelRadius);
        }
    }
    suspension::BalanceCar(car, *spec.frontWeightShare);

    // The body's inertia as a uniform box of the data's size (Assetto Corsa's INERTIA is the box's
    // width, height and length), of the sprung mass.
    const glm::vec3 box = spec.inertiaBox.value_or(glm::vec3(1.8f, 1.2f, 4.5f));
    car.rollInertia = car.sprungMass / 12.0 * (box.x * box.x + box.y * box.y);
    car.pitchInertia = car.sprungMass / 12.0 * (box.y * box.y + box.z * box.z);
    // The centre of mass's height: each axle's tyre radius plus its centre of mass's height over the
    // wheel centre (-BASEY), by the weight on it.
    const auto height = [](const VehicleSuspensionAxle& axle) {
        return static_cast<double>(axle.tyreRadius + axle.centerOfMassAboveWheel);
    };
    car.cgHeight = *spec.frontWeightShare * height(*spec.frontSuspension) + (1.0f - *spec.frontWeightShare) * height(*spec.rearSuspension);
    car.rackAtLock = FitSteeringRackTravel(settings);
    car.steeringWheelLockDegrees = spec.steeringWheelLockDegrees.value_or(settings.maxSteerAngleDegrees * 15.0f);
    return car;
}
}
