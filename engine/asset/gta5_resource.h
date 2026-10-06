#pragma once

#include "rage_resource.h"
#include "texture_loader.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

// GTA V Enhanced ("gen9") resources as the game ships them: texture dictionaries (.ytd) and fragments
// (.yft, a vehicle's drawable with its skeleton and breakable parts). The layouts are the game's;
// CodeWalker's gen9 support (dexyfex/CodeWalker, CodeWalker.Core/GameFiles/Resources) is where they
// were looked up. Only what an import needs is read: no physics bounds, cloth, lights or damage
// variants. The original PC release's (gen8) layouts differ in the shaders, textures and vertex
// buffers and are refused.

// The resource versions the reader takes.
inline constexpr std::uint32_t kGta5Gen9FragmentVersion = 171;
inline constexpr std::uint32_t kGta5Gen9TextureDictionaryVersion = 5;

// One texture: its base level only (an import writes a PNG and the engine makes its own mips).
struct Gta5Texture
{
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 0;
    // A DXGI_FORMAT (gen9 stores the game's own enumeration, which numbers formats as DXGI does).
    std::uint32_t format = 0;
    // What the texture is for (rage's TextureUsage: 20 diffuse, 22 normal, 23 specular, ...).
    std::uint8_t usage = 0;
    std::vector<std::uint8_t> baseLevel;
};

// A shader instance: its name and the textures and constants its parameters are set to, keyed by the
// parameter name's JenkinsHash (gen9 names: "DiffuseTex", "SpecularColor", ...).
struct Gta5Shader
{
    std::uint32_t nameHash = 0;
    // The name when it is one of the shaders the game's vehicles use, else "0x" and the hash.
    std::string name;
    std::uint8_t renderBucket = 0;
    std::map<std::uint32_t, std::string> textures;
    std::map<std::uint32_t, std::vector<float>> constants;

    // The texture name a parameter samples; empty when the parameter is unset or absent.
    std::string Texture(std::string_view parameter) const;
    // A constant parameter's floats; nullopt when absent.
    std::optional<std::vector<float>> Constant(std::string_view parameter) const;
};

struct Gta5Bone
{
    std::string name;
    std::uint16_t tag = 0;
    int parent = -1;
    // Relative to the parent bone.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 translation{0.0f};
    glm::vec3 scale{1.0f};
};

// One draw call's vertices and triangles. Every attribute array is either empty or one per vertex.
struct Gta5Geometry
{
    std::uint16_t shaderIndex = 0;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
    std::vector<glm::vec4> colors;
    // TEXCOORD0..; a set the vertices lack is empty.
    std::array<std::vector<glm::vec2>, 4> texCoords;
    // The skeleton bone each vertex follows: its heaviest blend weight's bone for a skinned model
    // (vehicles bind every vertex rigidly to one), the model's bone otherwise.
    std::vector<std::uint16_t> bones;
    std::vector<std::uint32_t> indices;
};

struct Gta5Model
{
    std::uint8_t boneIndex = 0;
    bool skinned = false;
    std::vector<Gta5Geometry> geometries;
};

struct Gta5Drawable
{
    std::vector<Gta5Shader> shaders;
    std::vector<Gta5Bone> bones;
    // High, medium, low and very low detail.
    std::array<std::vector<Gta5Model>, 4> lods;
    // The textures the drawable carries itself.
    std::vector<Gta5Texture> textures;
};

// A breakable part of a fragment: a vehicle's wheel, door or panel. The vehicle's own drawable
// already draws most of them; a part that brings a drawable (the wheels) is drawn at its bone with
// the fragment drawable's shaders.
struct Gta5FragmentChild
{
    std::uint16_t boneTag = 0;
    std::optional<Gta5Drawable> drawable;
};

struct Gta5Fragment
{
    std::string name;
    Gta5Drawable drawable;
    std::vector<Gta5FragmentChild> children;
};

namespace Gta5Resource
{
// A gen9 .yft. Throws std::runtime_error for another version or a corrupt file.
Gta5Fragment LoadFragment(const std::filesystem::path& path);
Gta5Fragment ReadFragment(const RageResource& resource);

// A gen9 .ytd. Throws std::runtime_error for another version or a corrupt file.
std::vector<Gta5Texture> LoadTextureDictionary(const std::filesystem::path& path);
std::vector<Gta5Texture> ReadTextureDictionary(const RageResource& resource, std::uint64_t address);

// The texture's base level as RGBA8, rows top-down. Throws std::runtime_error for a format the DDS
// decoder does not read.
TextureData DecodeTexture(const Gta5Texture& texture);

// The bytes of a base level of this size and format; 0 for a format the reader does not know.
size_t BaseLevelSize(std::uint32_t format, std::uint32_t width, std::uint32_t height);

// The vehicle shader names the reader resolves Gta5Shader::name for.
std::string_view ShaderName(std::uint32_t nameHash);
}

}
