#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace me
{

// Imports Assetto Corsa .kn5 cars and tracks by converting them, once, into a glTF 2.0 bundle the
// engine's glTF loader reads: "<name>.gltf", "buffers/<name>.bin" and "textures/*.png". A port of
// assetto-corsa-gltf's converter (https://github.com/semiloker/assetto-corsa-gltf, MIT), keeping
// what makes its output right rather than merely loadable:
//
//  - the node hierarchy one for one (a car's empty nodes are its wheel centres, suspension
//    pickups and hinges), under a root that turns AC's axes (+X left, +Z forward) into glTF's;
//  - the livery: textures come from the car's skins/<name>/ folder, as the game loads them;
//  - AC's Blinn-Phong parameters as metallic-roughness: the specular exponent and intensity
//    become roughness, txMaps' specular mask a per-pixel roughness, a flat txDetail the paint
//    colour, fresnelMaxLevel KHR_materials_specular and car paint's sun lobe
//    KHR_materials_clearcoat;
//  - runtime variants (*_BLUR, *_DAMAGE, the low-res half of an in-file LOD pair) dropped;
//  - CSP-encrypted files refused: their plain section is decoys.
struct Kn5ImportOptions
{
    // A folder under the car's skins/, matched ignoring case. Empty takes the first one, which is
    // what the game picks; "none" keeps the textures embedded in the kn5 (the export-time
    // template, usually grey primer).
    std::string skin;
    // Keep *_BLUR, *_DAMAGE and low-res LOD twins instead of dropping them.
    bool keepVariants = false;
    // Negate V. kn5 and glTF share a top-left UV origin, so this is for mods that arrive flipped.
    bool flipUv = false;
};

struct Kn5ImportReport
{
    std::filesystem::path gltfPath;
    // The skin folder the textures came from; empty when the kn5's own were kept.
    std::string skin;
    size_t skinTextures = 0;
    size_t nodes = 0;
    size_t transforms = 0;
    size_t meshes = 0;
    size_t emptyMeshes = 0;
    size_t droppedVariants = 0;
    size_t triangles = 0;
    size_t images = 0;
    size_t materials = 0;
    size_t foldedTextureNames = 0;
    size_t scrubbedAttributes = 0;
    size_t scrubbedMatrices = 0;
};

namespace Kn5Importer
{
bool IsKn5Path(const std::filesystem::path& path);

// The liveries next to a car: the folder names under "<kn5 folder>/skins", in the order the
// game offers them. Empty for a track or a car without skins.
std::vector<std::string> ListSkins(const std::filesystem::path& kn5Path);

// Converts `kn5Path` into targetDirectory (created if missing) and returns what was written.
// Throws std::runtime_error for an encrypted, corrupt or unreadable kn5, an unknown skin, or a
// destination glTF that already exists (an import never overwrites).
Kn5ImportReport ConvertToGltf(
    const std::filesystem::path& kn5Path,
    const std::filesystem::path& targetDirectory,
    const Kn5ImportOptions& options = {});

// ---- The rules, exposed for tests --------------------------------------------------------------

// *_BLUR and *_DAMAGE: meshes the game swaps in at runtime (ignoring case).
bool IsRuntimeVariant(const std::string& nodeName);

// The low-res halves of in-file LOD pairs: names ending "_LR" whose "_HR" twin is also present.
// The twin test matters: "_LR" means left-rear far more often (WHEEL_LR, SUSP_LR), and those have
// no "_HR" twin.
std::set<std::string> LowResTwins(const std::vector<std::string>& nodeNames);

// A Blinn-Phong exponent (already scaled by the specular intensity) as GGX roughness,
// sqrt(2 / (n + 2)), clamped to [0.04, 1].
float SpecularExponentToRoughness(float exponent);

// A texture that is one colour everywhere (every channel within 6 levels), as AC's txDetail
// paints it: the colour doubled (a detail map is neutral at mid-grey) in gamma space, then made
// linear. nullopt for a map that varies - a grain or a flake pattern, not a paint colour.
std::optional<std::array<float, 3>> FlatDetailTint(const std::vector<std::uint8_t>& rgba, int width, int height);
}
}
