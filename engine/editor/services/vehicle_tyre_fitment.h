#pragma once

#include <engine/physics/vehicle_settings.h>

#include <array>
#include <filesystem>
#include <string>

namespace me::VehicleTyreFitment
{

using Fitment = std::array<VehicleTyreRef, kVehicleWheelCount>;

// Fits `tyre` to `wheel` (0 front left, 1 front right, 2 rear left, 3 rear right), and with pairAxles to the
// other wheel of its axle too. A wheel given the tyre the car already names for it is left unfitted.
void Fit(Fitment& fitment, const VehicleCarSpec& car, size_t wheel, const VehicleTyreRef& tyre, bool pairAxles);

// Whether any wheel has a tyre fitted.
bool Any(const Fitment& fitment);

// The car with the fitted tyres named on its wheels, written into its glTF (`gltfPath`) and resolved from
// the library; the model cache's copy is updated too. Throws as TyreLibrary::WriteCarTyres does.
VehicleCarSpec Save(const std::filesystem::path& gltfPath, const VehicleCarSpec& car, const Fitment& fitment);

// A car imported before the tyre library: its own compounds put in the library (under `carFolder`) and
// named on its wheels, written and cached as Save does.
VehicleCarSpec Adopt(const std::filesystem::path& gltfPath, const VehicleCarSpec& car, const std::string& carFolder);
}
