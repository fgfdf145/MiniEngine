#pragma once

#include <engine/asset/acd_archive.h>
#include <engine/physics/vehicle_settings.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace me
{

// A line for a log or a panel: what the spec covers ("1460 kg, RWD, 386 Nm, 7500 rpm, 7 gears").
std::string DescribeCarSpec(const VehicleCarSpec& spec);

// What an Assetto Corsa car's data folder says about how it drives, as far as the physics engine has
// a place for it: the mass, the drive, the engine's torque and revs, the gearbox, the steering lock,
// the brakes and the springs, in SI units. The rest (tyre model, aero, turbo lag, damage) has none.
namespace AcCarData
{
// An ini file: upper-case section, then upper-case key, to the value with its comment cut off.
using Ini = std::map<std::string, std::map<std::string, std::string>>;
Ini ParseIni(const std::string& text);

// A lut file: "rpm|value" lines, in file order. Lines that are not two numbers are skipped.
std::vector<std::pair<float, float>> ParseLut(const std::string& text);

// Builds the spec from the data files by lower-case name (car.ini, engine.ini, power.lut,
// drivetrain.ini, brakes.ini, suspensions.ini). A file that is missing, or a value that is not a
// number, leaves its fields empty.
VehicleCarSpec BuildSpec(const AcdArchive::Files& files);

// The steady boost of a turbo at an rpm as a fraction of the engine's own torque: its maximum boost
// scaled by min(1, (rpm / reference)^gamma), cut at its wastegate when it has one (0: none).
float TurboBoost(float rpm, float maxBoost, float wastegate, float referenceRpm, float gamma);

// Reads the car in a folder: its data.acd, or an unpacked data/ folder beside it. nullopt when it has
// neither, or (with the reason in `problem`) when they cannot be read.
std::optional<VehicleCarSpec> ReadCarFolder(const std::filesystem::path& carFolder, std::string* problem = nullptr);
}
}
