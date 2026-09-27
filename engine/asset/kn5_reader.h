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
    // The file as stored: DDS almost always, occasionally PNG or JPEG. A stub (under 128 bytes)
    // is a placeholder, and on an encrypted model a decoy.
    std::vector<std::uint8_t> data;
};

struct Kn5Material
{
    std::string name;
    std::string shader;
    // Scalar shader parameters (ksSpecular, ksSpecularEXP, useDetail, fresnelMaxLevel, ...).
    std::map<std::string, float> properties;
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
    Kn5Node root;
    // The Custom Shaders Patch encryption trailer is present: textures and several meshes in the
    // plain section are decoys (1x1 images, 6 cm cubes).
    bool encrypted = false;
};

namespace Kn5Reader
{
// The trailer a CSP-encrypted file carries.
inline constexpr const char* kEncryptionMarker = "__AC_SHADERS_PATCH_KN5ENC_v1__";

// Parses a kn5 held in memory. With `readGeometry` false the vertex and index blocks are skipped
// (counts are still set). Throws std::runtime_error, naming `source`, for anything that is not a
// well-formed kn5.
Kn5Model Parse(const std::vector<std::uint8_t>& bytes, const std::string& source, bool readGeometry = true);

// Reads and parses a file. Throws std::runtime_error when it cannot be read or parsed.
Kn5Model Load(const std::filesystem::path& path, bool readGeometry = true);
}
}
