#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
//  - AC's Blinn-Phong parameters as metallic-roughness (docs/design/
//    2026-10-06-car-paint-correctness-design.md): the exponent, times txMaps' gloss, becomes
//    roughness; ksSpecular or fresnelMaxLevel, masked by txMaps, KHR_materials_specular; a flat
//    txDetail the paint colour; car paint's reflection KHR_materials_clearcoat;
//  - runtime variants (*_BLUR, *_DAMAGE, the low-res half of an in-file LOD pair) dropped;
//  - how the game draws each mesh, as MINIENGINE_mesh_draw: the ones that cast no shadow, and a
//    track's LOD distances (lodIn, lodOut), so its far LODs take over where the game's do;
//  - CSP-encrypted files refused: their plain section is decoys;
//  - a track's layout: models.ini / models_<layout>.ini place several kn5 in one scene, and the
//    import converts them all, each at its POSITION and ROTATION;
//  - a car's own data: the data.acd (or unpacked data/) beside the kn5 is read for the figures the
//    physics has a place for and written as MINIENGINE_vehicle (see AcCarData).
// Invoked from the importing thread with the overall import fraction in [0, 1], never going
// backwards. Implementations must be cheap and thread-safe (typically an atomic store).
using ImportProgressCallback = std::function<void(float)>;

struct Kn5ImportOptions
{
    // A folder under the car's skins/, matched ignoring case. Empty takes the first one, which is
    // what the game picks; "none" keeps the textures embedded in the kn5 (the export-time
    // template, usually grey primer). Ignored for a track layout: tracks have no skins.
    std::string skin;
    // Keep *_BLUR, *_DAMAGE and low-res LOD twins instead of dropping them.
    bool keepVariants = false;
    // Negate V. kn5 and glTF share a top-left UV origin, so this is for mods that arrive flipped.
    bool flipUv = false;
};

struct Kn5ImportReport
{
    std::filesystem::path gltfPath;
    // The kn5 files converted: one, or every model of a layout.
    size_t models = 0;
    // The skin folder the textures came from; empty when the kn5's own were kept.
    std::string skin;
    size_t skinTextures = 0;
    size_t nodes = 0;
    size_t transforms = 0;
    size_t meshes = 0;
    size_t emptyMeshes = 0;
    size_t droppedVariants = 0;
    // Meshes the game never draws (isRenderable off, or a track marker's cube), which are not imported,
    // except:
    size_t hiddenMeshes = 0;
    // a track's physics meshes, which are imported as collision only (MINIENGINE_collision).
    size_t collisionMeshes = 0;
    size_t collisionTriangles = 0;
    // Drawn meshes the game casts no shadow from, and those it draws only between two camera
    // distances (a track's LODs and small props), as MINIENGINE_mesh_draw carries them.
    size_t shadowlessMeshes = 0;
    size_t distanceLimitedMeshes = 0;
    size_t triangles = 0;
    size_t images = 0;
    size_t materials = 0;
    // The figures of the car's data.acd (or data folder) that were imported as MINIENGINE_vehicle, in
    // words ("1460 kg, RWD, 386 Nm, 7500 rpm, 7 gears"); empty when the kn5 has no data beside it.
    std::string carData;
    // Why a data.acd next to the kn5 was not imported (a renamed folder does not decrypt).
    std::string carDataProblem;
    // The car's FMOD sound bank was imported (WAVs under sounds/ and <name>.sounds.yaml), or why not.
    bool carSounds = false;
    std::string carSoundsProblem;
    size_t foldedTextureNames = 0;
    size_t scrubbedAttributes = 0;
    size_t scrubbedMatrices = 0;
};

// One livery as an import dialog offers it.
struct Kn5SkinSummary
{
    // The folder name under skins/; empty for the textures embedded in the kn5.
    std::string name;
    // The bodywork's paint as 8-bit sRGB: the flat txDetail colour of the most body-like painted
    // material, from this livery where it ships the texture, else from the kn5. nullopt when the
    // car has no painted material or its paint is a pattern rather than a colour.
    std::optional<std::array<std::uint8_t, 3>> paint;
    // The material the paint was read from.
    std::string paintMaterial;
    // The livery ships its own copy of the paint texture (otherwise every livery shows the kn5's).
    bool paintFromSkin = false;
};

// What an import dialog shows before converting: read without the vertex and index data.
struct Kn5ModelSummary
{
    bool encrypted = false;
    size_t meshes = 0;
    size_t triangles = 0;
    size_t materials = 0;
    size_t textures = 0;
    // Subtrees the default import drops (*_BLUR, *_DAMAGE, low-res LOD twins).
    size_t runtimeVariants = 0;
    // Meshes the game never draws, such as a track's physics surfaces: never imported, and not
    // among the meshes and triangles above.
    size_t hiddenMeshes = 0;
    // The kn5 files read: one, or every model of a layout. The counts above are their sums.
    size_t models = 0;
    // The skins/ folders in the order the game offers them (the first is the default), then the
    // embedded textures (an empty name) last. Only the embedded entry for a track layout.
    std::vector<Kn5SkinSummary> skins;
    // For a kn5: the track layouts beside it that place it, as an import dialog offers them.
    struct Layout
    {
        std::filesystem::path path;
        size_t models = 0;
    };
    std::vector<Layout> layouts;
};

// One model of a track layout: a kn5 and where the game places it.
struct Kn5LayoutModel
{
    std::filesystem::path file;
    // In AC's frame, metres.
    std::array<float, 3> position{};
    // Degrees about X, Y and Z, applied X first.
    std::array<float, 3> rotationDegrees{};
};

// A surface of a track's surfaces.ini: what a physics mesh named after it is made of.
struct Kn5Surface
{
    std::string key;
    float friction = 0.8f;
};

// Friction of a physics mesh whose surface no surfaces.ini defines (Spa's "WALL").
inline constexpr float kUnknownSurfaceFriction = 0.8f;

namespace Kn5Importer
{
bool IsKn5Path(const std::filesystem::path& path);
// A track layout: "models.ini" or "models_<layout>.ini", ignoring case.
bool IsLayoutPath(const std::filesystem::path& path);

// The name an import gives its bundle: the kn5's stem, or for a layout the track folder's name,
// with "_<layout>" for models_<layout>.ini.
std::string ImportName(const std::filesystem::path& source);

// The surfaces a track's physics meshes are made of, for an import of `source` (a kn5 or a
// layout): a models_<layout>.ini's <track>/<layout>/data/surfaces.ini, then the track's
// data/surfaces.ini, then the game's own (system/data/surfaces.ini, above content/tracks/<track>),
// then the four keys every install has. The first definition of a key is the one a mesh gets.
std::vector<Kn5Surface> LoadTrackSurfaces(const std::filesystem::path& source);

// The models a layout places, in file order. Each [MODEL_n] section with a FILE counts; FILE is
// relative to the ini, POSITION and ROTATION default to zero (and to zero when malformed).
// Throws std::runtime_error when the ini cannot be read, places nothing, or names a missing file.
std::vector<Kn5LayoutModel> ReadLayout(const std::filesystem::path& layoutPath);

// The layouts in a kn5's folder that place it, sorted by file name.
std::vector<std::filesystem::path> FindLayouts(const std::filesystem::path& kn5Path);

// Reads what an import dialog needs to offer the liveries and options, from a kn5 or a layout
// (summed over its models). Throws std::runtime_error for a corrupt or unreadable file; an
// encrypted one is reported, not thrown.
Kn5ModelSummary Inspect(const std::filesystem::path& source);

// The liveries next to a car: the folder names under "<kn5 folder>/skins", in the order the
// game offers them. Empty for a track or a car without skins.
std::vector<std::string> ListSkins(const std::filesystem::path& kn5Path);

// Converts a kn5, or every model of a layout, into targetDirectory (created if missing) as one
// glTF named ImportName(source), and returns what was written. Throws std::runtime_error for an
// encrypted, corrupt or unreadable kn5, an unknown skin, a layout that ReadLayout rejects, or a
// destination glTF that already exists (an import never overwrites). `progress` hears how far
// the conversion got.
Kn5ImportReport ConvertToGltf(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory,
    const Kn5ImportOptions& options = {},
    const ImportProgressCallback& progress = {});

// ---- The rules, exposed for tests --------------------------------------------------------------

// *_BLUR and *_DAMAGE: meshes the game swaps in at runtime (ignoring case).
bool IsRuntimeVariant(const std::string& nodeName);

// The track's spawn and timing points: AC_START_n, AC_PIT_n, AC_HOTLAP_START_n and AC_TIME_n_L/R
// (ignoring case). Their unit cubes are never drawn; the dummies of the same name are kept.
bool IsTrackMarker(const std::string& nodeName);

// A track's physics mesh: a mesh the game never draws whose name starts with a digit, then the
// surface's KEY ("1ROAD", "20ASPH-SPA_BLACK_004"). The game collides cars with these alone.
bool IsPhysicsMeshName(const std::string& nodeName);

// The [SURFACE_n] sections' KEY and FRICTION of a surfaces.ini, in file order.
std::vector<Kn5Surface> ParseSurfaces(const std::string& iniText);

// The surface a physics mesh is on: the longest KEY its name (leading digits dropped) starts with,
// ignoring case, the earlier surface winning a tie. When none does, the name without its leading
// and trailing digits, at kUnknownSurfaceFriction.
Kn5Surface MatchSurface(const std::vector<Kn5Surface>& surfaces, const std::string& nodeName);

// The low-res halves of in-file LOD pairs: names ending "_LR" whose "_HR" twin is also present.
// The twin test matters: "_LR" means left-rear far more often (WHEEL_LR, SUSP_LR), and those have
// no "_HR" twin.
std::set<std::string> LowResTwins(const std::vector<std::string>& nodeNames);

// A layout model's placement as a column-major matrix in AC's frame: rotation Z * Y * X, then the
// translation.
std::array<float, 16> LayoutModelMatrix(const std::array<float, 3>& position, const std::array<float, 3>& rotationDegrees);

// A Blinn-Phong exponent as glTF (perceptual) GGX roughness: alpha = sqrt(2 / (n + 2)), and
// roughness = sqrt(alpha), clamped to [0.04, 1].
float SpecularExponentToRoughness(float exponent);

// The painted materials (a txDetail slot the shader samples, useDetail > 0), most body-like first:
// a name that says paint, body or chassis, then an unrecognised one, then one that says rim,
// glass, interior and the like; interior copies after exterior ones; more triangles first within
// that. Returns material indices. Triangle count alone picks the rims on many cars.
std::vector<size_t> RankPaintedMaterials(
    const std::vector<std::string>& materialNames,
    const std::vector<bool>& painted,
    const std::vector<size_t>& triangles);

// A texture whose colour is one everywhere (every channel within 6 levels), as AC's txDetail
// paints it: the colour in gamma space, which AC multiplies into the diffuse as it is. nullopt for
// a map that varies - a grain or a weave, not a paint colour. The alpha is not looked at.
std::optional<std::array<float, 3>> FlatDetailColor(const std::vector<std::uint8_t>& rgba, int width, int height);

// How bright AC draws a surface's diffuse (docs/design/2026-10-06-ac-light-scale-design.md):
// ksDiffuse and ksAmbient weighed by the light each meets under the game's clear-noon weather, as a
// gain in gamma space; 2 at 0.5 and 0.5, where Kunos' car paint sits and the level its paint has
// always been imported at.
float DiffuseGain(float ksDiffuse, float ksAmbient);

// The cosine-weighted mean, over the hemisphere, of the weight AC gives a surface's cube-map
// reflection: min(fresnelC + (1 - N.V)^fresnelEXP, fresnelMaxLevel), in gamma space. isAdditive 0
// raises the exponent to at least 1, as the shader does.
float MeanReflection(float fresnelC, float fresnelExp, float fresnelMax, int isAdditive);
}
}
