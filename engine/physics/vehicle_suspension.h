#pragma once

#include "vehicle_settings.h"

#include <engine/suspension/suspension_axle.h>
#include <engine/suspension/suspension_friction.h>
#include <engine/suspension/suspension_model.h>
#include <engine/suspension/suspension_rigs.h>
#include <engine/suspension/suspension_strut.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace me
{

// One wheel of a car with a multibody suspension (VehicleSettings::frontSuspension/rearSuspension),
// ready for engine/suspension. The corner's frame is the suspension library's: x forward, y left,
// z up, metres, from the wheel centre at the design position (where the car rests, or with a rod
// length the hardpoints' reference, VehicleRestTravel from the rest).
struct VehicleCornerSetup
{
    suspension::SuspensionDefinition definition;
    // The springs, damper, stops and friction acting on the wheel travel (rates at the wheel).
    suspension::StrutUnitSettings unit;
    bool hasFriction = false;
    suspension::LuGreParameters friction;
    double antiRollBarRate = 0.0; // N/m of travel difference with the other wheel of the axle
    double hubMass = 0.0;
    double bumpTravel = 0.0;      // compression room to the hard limit, m
    double droopTravel = 0.0;     // extension room to full droop, m
    bool front = true;
    bool left = true;
};

// `staticLoad` is the wheel's share of the car's weight (N): the spring's preload, so the car
// rests at the design position, unless the axle has a rod length (Assetto Corsa's ROD_LENGTH): the
// spring is then compressed by it at the design position and the car rests where its load is.
VehicleCornerSetup BuildVehicleCorner(const VehicleSettings& settings, size_t wheelIndex, double staticLoad);

// Whether an axle's springs follow its rod length (an independent axle with ROD_LENGTH in its data).
inline bool HasRodLength(const VehicleSuspensionAxle& axle)
{
    return axle.rodLength.has_value() && axle.type != VehicleSuspensionType::None && axle.type != VehicleSuspensionType::SolidAxle;
}

// Where a wheel rests (m of travel from the design position, bump positive) under `springLoad` (N, its
// share of the weight less its hub's): where wheelRate (z + L) + progressiveRate (z + L)^2 / 2 carries
// it with a rod length L, the design position without one.
double VehicleRestTravel(const VehicleSuspensionAxle& axle, double springLoad);

// How far the wheel centre has moved from the design position at its rest travel (vehicle space): the
// design position is where the model draws the wheel less this.
glm::vec3 VehicleRestWheelOffset(const VehicleSettings& settings, size_t wheelIndex, double springLoad);

// The wheel's force unit with its friction (none when the setup has none).
suspension::StrutUnit MakeVehicleCornerUnit(const VehicleCornerSetup& setup);

// A solid axle's links and layout (the axle's type is SolidAxle), with the wheels' tyre radius.
suspension::SolidAxleDefinition BuildSolidAxle(const VehicleSuspensionAxle& axle, double tyreRadius);

// The rack travel (m) at full steering lock: the settings' own, or the one that turns the front
// wheels by maxSteerAngleDegrees on average (outer and inner wheel). Its sign makes positive
// steering (right) turn the wheels right.
double FitSteeringRackTravel(const VehicleSettings& settings);

// The whole car for the K&C and seven-post rigs (engine/suspension/suspension_rigs.h), from a car's
// own data: both axles' linkage and rates, the wheelbase and weight split, the mass and its inertia
// box, the tyres' vertical rate. Throws std::invalid_argument when the spec lacks any of these.
suspension::CarModel BuildCarModel(const VehicleCarSpec& spec, const std::string& name);

// From the suspension frame of a corner to vehicle space (+X left, +Y up, +Z forward) and back.
inline glm::vec3 CornerToVehicle(const suspension::Vec3& v)
{
    return glm::vec3(static_cast<float>(v.y), static_cast<float>(v.z), static_cast<float>(v.x));
}
inline suspension::Vec3 VehicleToCorner(const glm::vec3& v)
{
    return suspension::Vec3(v.z, v.x, v.y);
}
}
