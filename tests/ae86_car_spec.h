#pragma once

#include <engine/physics/vehicle_settings.h>

namespace me::test
{

// Assetto Corsa's hardpoints (x towards the car's centre, y up, z forward from the wheel centre) as
// the import has them: (forward, outward, up).
inline glm::vec3 AcHardpoint(float x, float y, float z)
{
    return glm::vec3(z, -x, y);
}

// An [AXLE] link (x to the car's left, y up, z forward from the axle's centre) as the import has it:
// (forward, left, up).
inline VehicleAxleLink AcAxleLink(glm::vec3 car, glm::vec3 axle)
{
    return VehicleAxleLink{glm::vec3(car.z, car.x, car.y), glm::vec3(axle.z, axle.x, axle.y)};
}

// The Toyota AE86 as Assetto Corsa's ks_toyota_ae86 data gives it: 995 kg (and 30 l of fuel well
// behind), MacPherson struts in front, a live axle behind on four trailing links and a Panhard rod,
// its springs at 72 % of the half axle.
inline VehicleCarSpec MakeAe86Spec()
{
    VehicleCarSpec spec;
    spec.massKg = 995.0f;
    spec.fuelLitres = 30.0f;
    spec.fuelTankPosition = glm::vec3(0.0f, -0.15f, -1.45f);
    spec.wheelbase = 2.4f;
    spec.frontWeightShare = 0.53f;
    spec.inertiaBox = glm::vec3(1.40f, 1.30f, 4.62f);
    spec.drive = VehicleDrive::RearWheel;
    spec.antiRollBars = true;

    VehicleSuspensionAxle front;
    front.type = VehicleSuspensionType::MacPherson;
    front.strutTop = AcHardpoint(0.177f, 0.5262f, -0.018f);
    front.strutLower = AcHardpoint(0.135f, -0.1238f, 0.01125f);
    front.lowerFront = AcHardpoint(0.440f, -0.0688f, 0.425f);
    front.lowerRear = AcHardpoint(0.440f, -0.0688f, 0.0f);
    front.lowerBall = AcHardpoint(0.135f, -0.1238f, 0.01125f);
    front.tieInner = AcHardpoint(0.440f, -0.0688f, -0.100f);
    front.tieOuter = AcHardpoint(0.135f, -0.1238f, -0.100f);
    front.staticCamberDegrees = 0.65f;
    front.toeOutRodLength = 0.0001f;
    front.track = 1.355f;
    front.wheelRate = 18000.0f;
    front.bumpStopRate = 30000.0f;
    front.bumpStopTravel = 0.100f;
    front.reboundStopTravel = 0.045f;
    front.dampBump = 4500.0f;
    front.dampFastBump = 1434.0f;
    front.dampFastBumpThreshold = 0.080f;
    front.dampRebound = 4000.0f;
    front.dampFastRebound = 2089.0f;
    front.dampFastReboundThreshold = 0.120f;
    front.antiRollBarRate = 18000.0f;
    front.hubMass = 46.0f;
    front.tyreRadius = 0.2888f;
    front.tyreRate = 254573.0f;
    front.tyreDamping = 600.0f;
    front.centerOfMassAboveWheel = 0.25f; // BASEY -0.25

    VehicleSuspensionAxle rear;
    rear.type = VehicleSuspensionType::SolidAxle;
    rear.axleLinks = {
        AcAxleLink(glm::vec3(0.4933f, -0.020f, 0.498f), glm::vec3(0.4900f, -0.080f, 0.0f)),
        AcAxleLink(glm::vec3(-0.4933f, -0.020f, 0.498f), glm::vec3(-0.4900f, -0.080f, 0.0f)),
        AcAxleLink(glm::vec3(0.2488f, 0.075f, 0.2405f), glm::vec3(0.2488f, 0.020f, 0.0f)),
        AcAxleLink(glm::vec3(-0.2488f, 0.075f, 0.2405f), glm::vec3(-0.2488f, 0.020f, 0.0f)),
        AcAxleLink(glm::vec3(-0.435f, 0.010f, -0.100f), glm::vec3(0.435f, -0.070f, -0.110f)),
    };
    rear.axleSpringPosition = 0.72f;
    rear.axleTorqueReaction = -0.5f;
    rear.track = 1.350f;
    rear.wheelRate = 22000.0f;
    rear.bumpStopRate = 50000.0f;
    rear.bumpStopTravel = 0.120f;
    rear.reboundStopTravel = 0.160f;
    rear.dampBump = 1300.0f;
    rear.dampFastBump = 800.0f;
    rear.dampFastBumpThreshold = 0.080f;
    rear.dampRebound = 3400.0f;
    rear.dampFastRebound = 1200.0f;
    rear.dampFastReboundThreshold = 0.100f;
    rear.antiRollBarRate = 3200.0f;
    rear.hubMass = 100.0f;
    rear.tyreRadius = 0.2888f;
    rear.tyreRate = 254573.0f;
    rear.tyreDamping = 600.0f;
    rear.centerOfMassAboveWheel = 0.28f; // BASEY -0.28
    spec.frontSuspension = front;
    spec.rearSuspension = rear;
    return spec;
}
}
