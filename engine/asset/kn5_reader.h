#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace me
{

// Assetto Corsa's .kn5: the plain container its cars and tracks ship in - a texture table, a
// material table and a node tree carrying the geometry. The layout is community reverse-engineered;
// this follows assetto-corsa-gltf (https://github.com/semiloker/assetto-corsa-gltf), whose reader is
// verified against a whole stock car library.

enum class Kn5NodeType : std::int32_t
{
    Dummy = 1,
    Mesh = 2,
    Skinned = 3,
};

struct Kn5Texture
{
    std::string name;
    bool active = true;
    // The file's size as stored, read even when the data is skipped.
    size_t size = 0;
    // The file as stored: DDS almost always, occasionally PNG or JPEG. A stub (under 128 bytes)
    // is a placeholder, and on an encrypted model a decoy. Empty when the read skipped it.
    std::vector<std::uint8_t> data;
};

struct Kn5Material
{
    std::string name;
    std::string shader;
    // Scalar shader parameters (ksSpecular, ksSpecularEXP, useDetail, fresnelMaxLevel, ...).
    std::map<std::string, float> properties;
    // The same parameters' three-component values (valueC), which is where a float3 one keeps its
    // value: ksEmissive is a colour, and its scalar is 0 on a material that glows.
    std::map<std::string, std::array<float, 3>> vectors;
    // Slot name (txDiffuse, txNormal, txMaps, txDetail, ...) to texture name.
    std::map<std::string, std::string> textures;
    bool alphaBlend = false;
    bool alphaTested = false;

    float Property(const std::string& key, float fallback) const;
    std::string Texture(const std::string& slot) const;
};

struct Kn5Vertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<float, 3> tangent{};
};

struct Kn5Node
{
    Kn5NodeType type = Kn5NodeType::Dummy;
    std::string name;
    bool active = true;
    // A dummy's local transform: Direct3D row-major, which is the same 16 floats as glTF's
    // column-major for the same transform.
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Mesh and skinned nodes. A skinned mesh keeps its bind pose; bones and weights are skipped.
    std::vector<Kn5Vertex> vertices;
    std::vector<std::uint16_t> indices;
    std::uint32_t materialIndex = 0;
    // False for a mesh the game collides with but never draws, such as a track's physics surfaces.
    bool renderable = true;
    // False for a mesh that casts no shadow: a track's ground, grass, guard rails and far LODs.
    bool castShadows = true;
    // The camera distances, in metres, between which the game draws the mesh: lodIn above 0 for a
    // far LOD, lodOut 0 for no limit. A track's LOD pairs overlap by a few metres (0-25 and 23-250).
    float lodIn = 0.0f;
    float lodOut = 0.0f;
    // Counts, set even when the geometry itself was skipped.
    std::uint32_t vertexCount = 0;
    std::uint32_t triangleCount = 0;
    std::vector<Kn5Node> children;

    bool HasGeometry() const
    {
        return type != Kn5NodeType::Dummy;
    }
};

struct Kn5Model
{
    std::uint32_t version = 0;
    std::vector<Kn5Texture> textures;
    std::vector<Kn5Material> materials;
    // A default (childless dummy) node when the read skipped the tree.
    Kn5Node root;
};

// How much of a kn5 a read takes in; what it leaves out is seeked past, not read. A track's kn5
// runs to hundreds of megabytes, mostly texture data.
enum class Kn5ReadScope
{
    // The texture table (names and sizes) and the materials.
    Tables,
    // The texture table with the texture data, and the materials.
    Textures,
    // Everything but vertex and index data; their counts are kept.
    NoGeometry,
    Everything,
};

namespace Kn5Reader
{
// The trailer a CSP-encrypted file carries.
inline constexpr const char* kEncryptionMarker = "__AC_SHADERS_PATCH_KN5ENC_v1__";

// Parses a kn5 held in memory. Throws std::runtime_error, naming `source`, for anything that is
// not a well-formed kn5.
Kn5Model Parse(const std::vector<std::uint8_t>& bytes, const std::string& source, Kn5ReadScope scope = Kn5ReadScope::Everything);

// Reads a file, streaming it. Throws std::runtime_error when it cannot be read or parsed.
Kn5Model Load(const std::filesystem::path& path, Kn5ReadScope scope = Kn5ReadScope::Everything);

// Whether the Custom Shaders Patch encryption trailer is present: then the textures and several
// meshes in the plain section are decoys (1x1 images, 6 cm cubes). A separate scan of the whole
// file, so a read that does not need it does not pay for it.
bool IsEncrypted(const std::vector<std::uint8_t>& bytes);
bool IsEncrypted(const std::filesystem::path& path);
}
}
