#pragma once

#include <engine/physics/vehicle_settings.h>
#include <engine/tyre/tyre_spec.h>

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

// The shared tyre library: one tyre per "<name>.tyre.yaml" under the assets root's tyres/ folder, each
// with a uuid sidecar like any asset, so that any car can be fitted with any tyre
// (docs/design/2026-10-08-tyre-library-design.md).
namespace TyreLibrary
{
inline constexpr std::string_view kSuffix = ".tyre.yaml";

bool IsTyreFile(const std::filesystem::path& path);

// The library's folder: AssetsRoot() / "tyres".
std::filesystem::path Root();

// A tyre as its file holds it, and back. Numbers are written in their shortest exact form, so a tyre
// reads back equal to what was written; empty fields are left out.
std::string ToYaml(const tyre::TyreSpec& spec);
std::optional<tyre::TyreSpec> FromYaml(const std::string& text, std::string* problem = nullptr);
std::optional<tyre::TyreSpec> Load(const std::filesystem::path& path, std::string* problem = nullptr);
void Save(const std::filesystem::path& path, const tyre::TyreSpec& spec);

// How a car refers to a tyre file: its uuid and its path (relative to the project root when under it).
VehicleTyreRef RefFor(const std::filesystem::path& path);

// Puts `spec` in the library as "<folder>/<stem>.tyre.yaml" (folder relative to Root()), unless a tyre
// equal to it but for `source` is already somewhere in the library, which is then referred to instead.
// A tyre already at that name from the same source is updated in place; a different one gets "_2", "_3",
// ... added to the stem.
VehicleTyreRef Store(const tyre::TyreSpec& spec, const std::filesystem::path& folder, const std::string& stem);

// The tyre a car refers to, found by uuid, else by its path; nullopt when neither finds a readable one.
std::optional<tyre::TyreSpec> Resolve(const VehicleTyreRef& ref, std::filesystem::path* resolvedPath = nullptr);

struct Entry
{
    std::filesystem::path path;
    VehicleTyreRef ref;
    tyre::TyreSpec spec;
};
// Every readable tyre in the library, by path.
std::vector<Entry> List();

// A file-name stem from a compound name ("Slick Medium" -> "slick_medium").
std::string StemFor(const std::string& name);

// Puts the car's own compounds (spec.tyreCompounds) in the library under the folder `carFolder`, and
// fills spec.libraryCompounds and spec.wheelTyreRefs (the default compound on all four wheels). Nothing
// changes for a car without compounds.
void AdoptCarTyres(VehicleCarSpec& spec, const std::string& carFolder);

// MINIENGINE_vehicle's "tyres" member: {"compounds": [{"name", "front": ref, "rear": ref}], "wheels": [4
// refs]}, a ref being {"uuid", "path"}. Empty (null) when the car refers to no library tyre.
nlohmann::json CarTyresToJson(const VehicleCarSpec& spec);

// Writes the car's library tyres (spec.libraryCompounds, spec.wheelTyreRefs) into the MINIENGINE_vehicle of
// the glTF at `gltfPath`, leaving the rest of the file as it is. Throws when the file cannot be read or
// written or has no MINIENGINE_vehicle.
void WriteCarTyres(const std::filesystem::path& gltfPath, const VehicleCarSpec& spec);

// For a car imported before the library: puts the compounds its glTF carries (MINIENGINE_vehicle's
// tyreCompounds) in the library under `carFolder` (the game's folder name for the car) and writes the refs
// into the glTF, the default compound on all four wheels. Given the game's car folder itself (a directory),
// the compounds are read afresh from its data, and tyres stored from it before are updated in place. Returns how many tyres the car refers to; throws
// as WriteCarTyres does, or when the glTF carries no compounds.
size_t AdoptGltfCarTyres(const std::filesystem::path& gltfPath, const std::string& carFolder);

// Resolves spec.wheelTyreRefs into spec.wheelTyres; a wheel whose tyre is not found gets its default
// compound's from tyreCompounds. Returns the wheels that fell back (0 to 4), for the log.
int ResolveWheelTyres(VehicleCarSpec& spec);
}
}
