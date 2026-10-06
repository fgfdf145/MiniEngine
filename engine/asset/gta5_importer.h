#pragma once

#include "kn5_importer.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace me
{

// Imports a GTA V Enhanced vehicle (.yft) by converting it, once, into a glTF 2.0 bundle the engine's
// glTF loader reads: "<name>.gltf", "buffers/<name>.bin" and "textures/*.png", as the kn5 import lays
// it out. What makes it a car rather than a pile of triangles:
//
//  - the skeleton one node per bone (chassis, doors, bonnet, wheel_lf, hub_lf, steeringwheel, ...),
//    under a root that turns GTA's axes (+X right, +Y forward, +Z up) into glTF's (+X right,
//    -Z forward, +Y up); the body's vertices, which the game skins rigidly to those bones, are split
//    per bone into the bone's own frame, so a door is a node that can swing;
//  - the wheels, which the game draws from the fragment's wheel parts: the left front's mesh on every
//    corner that has none of its own, turned half a turn on the right;
//  - the most detailed model: "<name>_hi.yft" when it sits next to "<name>.yft";
//  - the textures from wherever the game finds them: the fragment's own, "<txd>+hi.ytd", "<txd>.ytd"
//    and the parents vehicles.meta's txdRelationships names, down to vehshare;
//  - the game's Blinn-Phong vehicle shaders as metallic-roughness, the paint slots (primary,
//    secondary, wheels, trim) coloured from the game's palette with each of carvariations.meta's colour
//    sets as a KHR_materials_variants variant, liveries (paint3's DiffuseTex2) baked over the paint.
// Only GTA V Enhanced (gen9) files are read; see Gta5Resource.

struct Gta5ImportOptions
{
    // Keep the extra_N parts (roof racks, light bars, spoilers the game switches per spawn).
    bool keepExtras = true;
};

struct Gta5ImportReport
{
    std::filesystem::path gltfPath;
    // The fragment converted (the _hi one when there is one).
    std::filesystem::path fragmentPath;
    // vehicles.meta's names for the model; empty when no vehicles.meta lists it.
    std::string gameName;
    std::string makeName;
    size_t nodes = 0;
    size_t meshes = 0;
    size_t primitives = 0;
    size_t triangles = 0;
    size_t materials = 0;
    size_t images = 0;
    size_t wheels = 0;
    size_t colorVariants = 0;
    size_t droppedExtras = 0;
    // Texture names a shader samples that no dictionary in reach has.
    std::vector<std::string> missingTextures;
};

namespace Gta5Importer
{
// A .yft (ignoring case).
bool IsGta5Path(const std::filesystem::path& path);

// The vehicle's model name: the file's stem without a trailing "_hi".
std::string ImportName(const std::filesystem::path& source);

// Converts the vehicle into targetDirectory (created if missing) as ImportName(source) + ".gltf" and
// returns what was written. Throws std::runtime_error for a file that is not a gen9 fragment, or a
// destination glTF that already exists (an import never overwrites).
Gta5ImportReport ConvertToGltf(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory,
    const Gta5ImportOptions& options = {},
    const ImportProgressCallback& progress = {});

// ---- The rules, exposed for tests --------------------------------------------------------------

// The palette slot a vehicle shader's DiffuseColor selects: the game marks a slot with x = 2 and
// numbers it in y (1 primary, 2 secondary, 3 pearlescent, 4 wheels, 6 interior trim, 7 dashboard);
// 0 for none (5 is the default: no colour).
int PaintSlot(const std::vector<float>& diffuseColor);

// The GTA perceptual roughness of a specular map's glossiness (its green channel, 0 to 1).
float GlossToRoughness(float gloss);
}

}
