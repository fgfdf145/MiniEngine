#pragma once

#include <engine/physics/vehicle_settings.h>

namespace me::test
{

// The Nissan GT-R GT3 as Assetto Corsa's ks_nissan_gtr_gt3 data gives it (the kn5 import's
// MINIENGINE_vehicle): 1375 kg, rear drive, double wishbones front and rear with their hardpoints,
// wheel rates, two-stage dampers, bump stops and anti-roll bars.
inline VehicleCarSpec MakeGtrSpec()
{
    VehicleCarSpec spec;
    spec.massKg = 1375.0f;
    spec.drive = VehicleDrive::RearWheel;
    spec.torqueCurve = {{500.0f, 155.0f}, {1500.0f, 219.0f}, {2500.0f, 299.0f}, {3500.0f, 582.0f}, {4000.0f, 584.0f}, {6000.0f, 578.0f}, {7000.0f, 500.0f}, {8000.0f, 0.0f}};
    spec.minRpm = 2100.0f;
    spec.maxRpm = 7000.0f;
    spec.gearRatios = {2.5f, 2.0f, 1.5217f, 1.2f, 1.0312f, 0.8857f};
    spec.reverseGearRatio = -3.818f;
    spec.finalDriveRatio = 3.4285f;
    spec.gearSwitchSeconds = 0.1f;
    spec.clutchReleaseSeconds = 0.1f;
    spec.engineInertia = 0.135f;
    spec.frontTyres = VehicleTyreSettings{1.5741f, 1.5748f, 0.1058f, 6.04f, 0.85f, 1.8f};
    spec.rearTyres = VehicleTyreSettings{1.6391f, 1.6435f, 0.1058f, 6.04f, 0.85f, 1.8f};
    spec.maxSteerAngleDegrees = 320.0f / 13.6f; // car.ini STEER_LOCK / STEER_RATIO
    spec.brakeTorquePerWheel = 1000.0f;
    spec.frontBrakeShare = 0.67f;
    spec.antiRollBars = true;
    spec.limitedSlipDifferentials = true;

    VehicleSuspensionAxle front;
    front.type = VehicleSuspensionType::DoubleWishbone;
    front.lowerFront = {0.01435f, -0.44595f, -0.1835f};
    front.lowerRear = {-0.34965f, -0.44995f, -0.1835f};
    front.lowerBall = {0.01839f, -0.11121f, -0.11753f};
    front.upperFront = {0.09135f, -0.38995f, 0.3445f};
    front.upperRear = {-0.13765f, -0.39595f, 0.2785f};
    front.upperBall = {-0.05316f, -0.2005f, 0.41825f};
    front.tieInner = {-0.1468f, -0.42095f, -0.15127f};
    front.tieOuter = {-0.1315f, -0.13095f, -0.08929f};
    front.staticCamberDegrees = -2.9f;
    front.toeOutRodLength = -0.0004f;
    front.track = 1.675f;
    front.wheelRate = 153000.0f;
    front.bumpStopRate = 150000.0f;
    front.bumpStopTravel = 0.055f;
    front.reboundStopTravel = 0.05f;
    front.dampBump = 10600.0f;
    front.dampFastBump = 3800.0f;
    front.dampFastBumpThreshold = 0.06f;
    front.dampRebound = 12375.0f;
    front.dampFastRebound = 5485.0f;
    front.dampFastReboundThreshold = 0.12f;
    front.antiRollBarRate = 68000.0f;
    front.hubMass = 59.0f;
    front.tyreRadius = 0.355f;
    front.tyreRate = 284861.0f;
    front.tyreDamping = 500.0f;

    VehicleSuspensionAxle rear;
    rear.type = VehicleSuspensionType::DoubleWishbone;
    rear.lowerFront = {0.32548f, -0.291262f, -0.094691f};
    rear.lowerRear = {0.10478f, -0.480002f, -0.159311f};
    rear.lowerBall = {0.03269f, -0.151896f, -0.093012f};
    rear.upperFront = {0.11759f, -0.425962f, 0.015709f};
    rear.upperRear = {-0.0792f, -0.501202f, 0.027289f};
    rear.upperBall = {0.04021f, -0.165066f, 0.150372f};
    rear.tieInner = {-0.1552f, -0.623202f, -0.128711f};
    rear.tieOuter = {-0.137522f, -0.157345f, 0.00194f};
    rear.staticCamberDegrees = -1.1f;
    rear.toeOutRodLength = 0.0011f;
    rear.track = 1.68f;
    rear.wheelRate = 125000.0f;
    rear.bumpStopRate = 120000.0f;
    rear.bumpStopTravel = 0.06f;
    rear.reboundStopTravel = 0.08f;
    rear.dampBump = 6017.0f;
    rear.dampFastBump = 2875.0f;
    rear.dampFastBumpThreshold = 0.06f;
    rear.dampRebound = 10770.0f;
    rear.dampFastRebound = 4600.0f;
    rear.dampFastReboundThreshold = 0.12f;
    rear.antiRollBarRate = 12050.0f;
    rear.hubMass = 66.0f;
    rear.tyreRadius = 0.355f;
    rear.tyreRate = 284861.0f;
    rear.tyreDamping = 500.0f;
    spec.frontSuspension = front;
    spec.rearSuspension = rear;
    return spec;
}
}
