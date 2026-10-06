#include "gta5_resource.h"

#include "dds_decoder.h"

#include <engine/core/text/ascii.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

namespace me
{

namespace
{
// rage::grcVertexDeclaration in gen9: one slot per semantic, each with the element's offset, the
// vertex stride and the element's format. The slots an import reads:
constexpr size_t kVertexSemanticCount = 52;
constexpr size_t kSemanticPosition = 0;
constexpr size_t kSemanticNormal = 4;
constexpr size_t kSemanticTangent = 8;
constexpr size_t kSemanticBlendWeights = 16;
constexpr size_t kSemanticBlendIndices = 20;
constexpr size_t kSemanticColor0 = 24;
constexpr size_t kSemanticTexCoord0 = 28;

// The element formats (numbered as DXGI_FORMAT) vehicles use.
constexpr std::uint8_t kElementFloat4 = 2;
constexpr std::uint8_t kElementFloat3 = 6;
constexpr std::uint8_t kElementHalf4 = 10;
constexpr std::uint8_t kElementFloat2 = 16;
constexpr std::uint8_t kElementUnorm10x3A2 = 24;
constexpr std::uint8_t kElementUnorm8x4 = 28;
constexpr std::uint8_t kElementUint8x4 = 30;
constexpr std::uint8_t kElementHalf2 = 34;

// A rage::pgObjectArray / atArray header: a pointer, then a 16-bit count and capacity.
struct ArrayHeader
{
    std::uint64_t pointer = 0;
    std::uint16_t count = 0;
    std::uint16_t capacity = 0;
};

ArrayHeader ReadArrayHeader(const RageResource& resource, std::uint64_t address)
{
    ArrayHeader header;
    header.pointer = resource.Read<std::uint64_t>(address);
    header.count = resource.Read<std::uint16_t>(address + 8);
    header.capacity = resource.Read<std::uint16_t>(address + 10);
    return header;
}

std::string HashText(std::uint32_t hash)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", hash);
    return text;
}

glm::vec4 ReadElement(const std::uint8_t* bytes, std::uint8_t format)
{
    float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (format)
    {
    case kElementFloat4:
        std::memcpy(values, bytes, 16);
        break;
    case kElementFloat3:
        std::memcpy(values, bytes, 12);
        break;
    case kElementFloat2:
        std::memcpy(values, bytes, 8);
        break;
    case kElementHalf4:
    case kElementHalf2:
        for (int index = 0; index < (format == kElementHalf4 ? 4 : 2); ++index)
        {
            std::uint16_t half;
            std::memcpy(&half, bytes + index * 2, 2);
            values[index] = glm::unpackHalf1x16(half);
        }
        break;
    case kElementUnorm8x4:
        for (int index = 0; index < 4; ++index)
        {
            values[index] = static_cast<float>(bytes[index]) / 255.0f;
        }
        break;
    case kElementUint8x4:
        for (int index = 0; index < 4; ++index)
        {
            values[index] = static_cast<float>(bytes[index]);
        }
        break;
    case kElementUnorm10x3A2:
    {
        std::uint32_t packed;
        std::memcpy(&packed, bytes, 4);
        values[0] = static_cast<float>(packed & 0x3FF) / 1023.0f;
        values[1] = static_cast<float>((packed >> 10) & 0x3FF) / 1023.0f;
        values[2] = static_cast<float>((packed >> 20) & 0x3FF) / 1023.0f;
        values[3] = static_cast<float>(packed >> 30) / 3.0f;
        break;
    }
    default:
        throw std::runtime_error("unsupported vertex element format " + std::to_string(format));
    }
    return {values[0], values[1], values[2], values[3]};
}

size_t ElementSize(std::uint8_t format)
{
    switch (format)
    {
    case kElementFloat4:
        return 16;
    case kElementFloat3:
        return 12;
    case kElementFloat2:
    case kElementHalf4:
        return 8;
    case kElementUnorm8x4:
    case kElementUint8x4:
    case kElementUnorm10x3A2:
    case kElementHalf2:
        return 4;
    default:
        return 0;
    }
}

Gta5Texture ReadTexture(const RageResource& resource, std::uint64_t address, bool withData)
{
    // rage::sga::Texture (gen9): the image parameters at 0x18, the name at 0x28, the texels at 0x38.
    Gta5Texture texture;
    texture.width = resource.Read<std::uint16_t>(address + 0x18);
    texture.height = resource.Read<std::uint16_t>(address + 0x1A);
    texture.format = resource.Read<std::uint8_t>(address + 0x1F);
    texture.mipLevels = resource.Read<std::uint8_t>(address + 0x22);
    texture.name = resource.ReadString(resource.Read<std::uint64_t>(address + 0x28));
    texture.usage = static_cast<std::uint8_t>(resource.Read<std::uint32_t>(address + 0x40) & 0x1F);
    const std::uint64_t data = resource.Read<std::uint64_t>(address + 0x38);
    if (withData && data != 0)
    {
        const size_t size = Gta5Resource::BaseLevelSize(texture.format, texture.width, texture.height);
        if (size != 0)
        {
            const std::uint8_t* bytes = resource.Bytes(data, size);
            texture.baseLevel.assign(bytes, bytes + size);
        }
    }
    return texture;
}

std::vector<Gta5Bone> ReadSkeleton(const RageResource& resource, std::uint64_t address)
{
    std::vector<Gta5Bone> bones;
    if (address == 0)
    {
        return bones;
    }
    const std::uint64_t bonesAddress = resource.Read<std::uint64_t>(address + 0x20);
    const std::uint16_t count = resource.Read<std::uint16_t>(address + 0x5E);
    bones.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index)
    {
        // rage::crBoneData, 0x50 bytes.
        const std::uint64_t bone = bonesAddress + static_cast<std::uint64_t>(index) * 0x50;
        Gta5Bone out;
        const glm::vec4 rotation = resource.Read<glm::vec4>(bone);
        out.rotation = glm::quat(rotation.w, rotation.x, rotation.y, rotation.z);
        out.translation = resource.Read<glm::vec3>(bone + 0x10);
        out.scale = resource.Read<glm::vec3>(bone + 0x20);
        out.parent = resource.Read<std::int16_t>(bone + 0x32);
        out.name = resource.ReadString(resource.Read<std::uint64_t>(bone + 0x38));
        out.tag = resource.Read<std::uint16_t>(bone + 0x44);
        if (out.parent >= static_cast<int>(count) || out.parent >= static_cast<int>(index))
        {
            out.parent = -1; // a parent comes before its children; anything else is not trusted
        }
        bones.push_back(std::move(out));
    }
    return bones;
}

Gta5Shader ReadShader(const RageResource& resource, std::uint64_t address)
{
    // rage::grmShader (gen9): the name, the constant buffers, the texture references and the
    // parameter table that says where each parameter lives.
    Gta5Shader shader;
    shader.nameHash = resource.Read<std::uint32_t>(address);
    const std::string_view known = Gta5Resource::ShaderName(shader.nameHash);
    shader.name = known.empty() ? HashText(shader.nameHash) : std::string(known);
    const std::uint64_t buffers = resource.Read<std::uint64_t>(address + 0x08);
    const std::uint64_t textureRefs = resource.Read<std::uint64_t>(address + 0x10);
    const std::uint64_t infos = resource.Read<std::uint64_t>(address + 0x20);
    shader.renderBucket = resource.Read<std::uint8_t>(address + 0x39);
    if (infos == 0)
    {
        return shader;
    }
    const std::uint8_t bufferCount = resource.Read<std::uint8_t>(infos);
    const std::uint8_t textureCount = resource.Read<std::uint8_t>(infos + 1);
    const std::uint8_t parameterCount = resource.Read<std::uint8_t>(infos + 4);
    for (std::uint8_t index = 0; index < parameterCount; ++index)
    {
        const std::uint32_t hash = resource.Read<std::uint32_t>(infos + 8 + index * 8u);
        const std::uint32_t data = resource.Read<std::uint32_t>(infos + 12 + index * 8u);
        const std::uint32_t type = data & 0x3;
        if (type == 0) // a texture
        {
            const std::uint32_t slot = (data >> 2) & 0xFF;
            if (textureRefs == 0 || slot >= textureCount)
            {
                continue;
            }
            const std::uint64_t texture = resource.Read<std::uint64_t>(textureRefs + slot * 8u);
            if (texture != 0)
            {
                const std::string name = resource.ReadString(resource.Read<std::uint64_t>(texture + 0x28));
                if (!name.empty())
                {
                    shader.textures[hash] = ToLowerAscii(name);
                }
            }
        }
        else if (type == 3) // a constant in one of the constant buffers
        {
            const std::uint32_t buffer = (data >> 2) & 0x3F;
            const std::uint32_t offset = (data >> 8) & 0xFFF;
            const std::uint32_t length = (data >> 20) & 0xFFF;
            if (buffers == 0 || buffer >= bufferCount || length < 4)
            {
                continue;
            }
            const std::uint64_t base = resource.Read<std::uint64_t>(buffers + buffer * 8u);
            if (base != 0)
            {
                shader.constants[hash] = resource.ReadArray<float>(base + offset, length / 4);
            }
        }
    }
    return shader;
}

Gta5Geometry ReadGeometry(const RageResource& resource, std::uint64_t address, const Gta5Model& model)
{
    Gta5Geometry geometry;
    const std::uint64_t vertexBuffer = resource.Read<std::uint64_t>(address + 0x18);
    const std::uint64_t indexBuffer = resource.Read<std::uint64_t>(address + 0x38);
    const std::uint16_t boneIdCount = resource.Read<std::uint16_t>(address + 0x72);
    const std::vector<std::uint16_t> boneIds =
        resource.ReadArray<std::uint16_t>(resource.Read<std::uint64_t>(address + 0x68), boneIdCount);
    if (vertexBuffer == 0 || indexBuffer == 0)
    {
        return geometry;
    }

    const std::uint32_t vertexCount = resource.Read<std::uint32_t>(vertexBuffer + 0x08);
    const std::uint16_t stride = resource.Read<std::uint16_t>(vertexBuffer + 0x0C);
    const std::uint64_t vertexData = resource.Read<std::uint64_t>(vertexBuffer + 0x18);
    const std::uint64_t declaration = resource.Read<std::uint64_t>(vertexBuffer + 0x38);
    const std::vector<std::uint32_t> offsets = resource.ReadArray<std::uint32_t>(declaration, kVertexSemanticCount);
    const std::vector<std::uint8_t> formats = resource.ReadArray<std::uint8_t>(declaration + kVertexSemanticCount * 5, kVertexSemanticCount);
    const std::uint8_t* vertices = resource.Bytes(vertexData, static_cast<size_t>(vertexCount) * stride);

    const auto read = [&](size_t semantic, auto&& store)
    {
        const std::uint8_t format = formats[semantic];
        if (format == 0)
        {
            return false;
        }
        const size_t size = ElementSize(format);
        if (size == 0 || offsets[semantic] + size > stride)
        {
            throw std::runtime_error("'" + resource.Source() + "' has a vertex element outside its vertex");
        }
        for (std::uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            store(ReadElement(vertices + static_cast<size_t>(vertex) * stride + offsets[semantic], format));
        }
        return true;
    };

    if (!read(kSemanticPosition, [&](const glm::vec4& value) { geometry.positions.emplace_back(value); }))
    {
        return geometry;
    }
    read(kSemanticNormal, [&](const glm::vec4& value) { geometry.normals.emplace_back(value); });
    read(kSemanticTangent, [&](const glm::vec4& value) { geometry.tangents.push_back(value); });
    read(kSemanticColor0, [&](const glm::vec4& value) { geometry.colors.push_back(value); });
    for (size_t set = 0; set < geometry.texCoords.size(); ++set)
    {
        read(kSemanticTexCoord0 + set, [&](const glm::vec4& value) { geometry.texCoords[set].emplace_back(value); });
    }

    // The bone of each vertex.
    std::vector<glm::vec4> weights;
    std::vector<glm::vec4> blendIndices;
    if (model.skinned)
    {
        read(kSemanticBlendWeights, [&](const glm::vec4& value) { weights.push_back(value); });
        read(kSemanticBlendIndices, [&](const glm::vec4& value) { blendIndices.push_back(value); });
    }
    geometry.bones.assign(vertexCount, model.boneIndex);
    if (weights.size() == vertexCount && blendIndices.size() == vertexCount)
    {
        for (std::uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            int heaviest = 0;
            for (int component = 1; component < 4; ++component)
            {
                if (weights[vertex][component] > weights[vertex][heaviest])
                {
                    heaviest = component;
                }
            }
            const size_t blend = static_cast<size_t>(blendIndices[vertex][heaviest]);
            const size_t bone = boneIds.empty() ? blend : (blend < boneIds.size() ? boneIds[blend] : 0);
            geometry.bones[vertex] = static_cast<std::uint16_t>(bone);
        }
    }

    const std::uint32_t indexCount = resource.Read<std::uint32_t>(indexBuffer + 0x08);
    const std::uint16_t indexSize = resource.Read<std::uint16_t>(indexBuffer + 0x0C);
    const std::uint64_t indexData = resource.Read<std::uint64_t>(indexBuffer + 0x18);
    if (indexSize == 4)
    {
        geometry.indices = resource.ReadArray<std::uint32_t>(indexData, indexCount);
    }
    else
    {
        const std::vector<std::uint16_t> indices = resource.ReadArray<std::uint16_t>(indexData, indexCount);
        geometry.indices.assign(indices.begin(), indices.end());
    }
    geometry.indices.resize(geometry.indices.size() - geometry.indices.size() % 3);
    for (const std::uint32_t index : geometry.indices)
    {
        if (index >= vertexCount)
        {
            throw std::runtime_error("'" + resource.Source() + "' has a triangle index past its vertices");
        }
    }
    return geometry;
}

std::vector<Gta5Model> ReadModels(const RageResource& resource, std::uint64_t listAddress)
{
    std::vector<Gta5Model> models;
    if (listAddress == 0)
    {
        return models;
    }
    const ArrayHeader list = ReadArrayHeader(resource, listAddress);
    for (std::uint16_t index = 0; index < list.count; ++index)
    {
        const std::uint64_t address = resource.Read<std::uint64_t>(list.pointer + index * 8u);
        if (address == 0)
        {
            continue;
        }
        // rage::grmModel.
        Gta5Model model;
        const std::uint64_t geometries = resource.Read<std::uint64_t>(address + 0x08);
        const std::uint16_t geometryCount = resource.Read<std::uint16_t>(address + 0x10);
        const std::uint64_t shaderMapping = resource.Read<std::uint64_t>(address + 0x20);
        const std::uint32_t binding = resource.Read<std::uint32_t>(address + 0x28);
        model.boneIndex = static_cast<std::uint8_t>(binding >> 24);
        model.skinned = ((binding >> 8) & 0xFF) != 0;
        const std::vector<std::uint16_t> shaders = resource.ReadArray<std::uint16_t>(shaderMapping, geometryCount);
        for (std::uint16_t geometryIndex = 0; geometryIndex < geometryCount; ++geometryIndex)
        {
            const std::uint64_t geometry = resource.Read<std::uint64_t>(geometries + geometryIndex * 8u);
            if (geometry == 0)
            {
                continue;
            }
            Gta5Geometry read = ReadGeometry(resource, geometry, model);
            read.shaderIndex = shaders[geometryIndex];
            if (!read.positions.empty() && !read.indices.empty())
            {
                model.geometries.push_back(std::move(read));
            }
        }
        models.push_back(std::move(model));
    }
    return models;
}

// A rage::rmcDrawable. A fragment child's drawable has no shaders or skeleton of its own: it uses its
// fragment's.
Gta5Drawable ReadDrawable(const RageResource& resource, std::uint64_t address)
{
    Gta5Drawable drawable;
    const std::uint64_t shaderGroup = resource.Read<std::uint64_t>(address + 0x10);
    const std::uint64_t skeleton = resource.Read<std::uint64_t>(address + 0x18);
    if (shaderGroup != 0)
    {
        const std::uint64_t textures = resource.Read<std::uint64_t>(shaderGroup + 0x08);
        const std::uint64_t shaders = resource.Read<std::uint64_t>(shaderGroup + 0x10);
        const std::uint16_t shaderCount = resource.Read<std::uint16_t>(shaderGroup + 0x18);
        for (std::uint16_t index = 0; index < shaderCount; ++index)
        {
            const std::uint64_t shader = resource.Read<std::uint64_t>(shaders + index * 8u);
            drawable.shaders.push_back(shader != 0 ? ReadShader(resource, shader) : Gta5Shader{});
        }
        if (textures != 0)
        {
            drawable.textures = Gta5Resource::ReadTextureDictionary(resource, textures);
        }
    }
    drawable.bones = ReadSkeleton(resource, skeleton);
    for (size_t lod = 0; lod < drawable.lods.size(); ++lod)
    {
        drawable.lods[lod] = ReadModels(resource, resource.Read<std::uint64_t>(address + 0x50 + lod * 8));
    }
    return drawable;
}

// The DDS bytes of a texture's base level, for DdsDecoder.
std::vector<std::uint8_t> BuildDds(const Gta5Texture& texture)
{
    std::vector<std::uint8_t> dds(4 + 124, 0);
    const auto put = [&](size_t offset, std::uint32_t value) { std::memcpy(dds.data() + offset, &value, 4); };
    std::memcpy(dds.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000);
    put(12, texture.height);
    put(16, texture.width);
    put(28, 1);
    put(76, 32);
    put(108, 0x1000);
    switch (texture.format)
    {
    case 61: // R8_UNORM: grey
        put(80, 0x20000);
        put(88, 8);
        put(92, 0xFF);
        break;
    case 65: // A8_UNORM
        put(80, 0x2);
        put(88, 8);
        put(104, 0xFF);
        break;
    case 85: // B5G6R5_UNORM
        put(80, 0x40);
        put(88, 16);
        put(92, 0xF800);
        put(96, 0x07E0);
        put(100, 0x001F);
        break;
    case 86: // B5G5R5A1_UNORM
        put(80, 0x40 | 0x1);
        put(88, 16);
        put(92, 0x7C00);
        put(96, 0x03E0);
        put(100, 0x001F);
        put(104, 0x8000);
        break;
    default:
    {
        put(80, 0x4);
        put(84, 0x30315844); // "DX10"
        std::uint32_t dx10[5] = {texture.format, 3, 0, 1, 0};
        dds.insert(dds.end(), reinterpret_cast<std::uint8_t*>(dx10), reinterpret_cast<std::uint8_t*>(dx10) + sizeof(dx10));
        break;
    }
    }
    dds.insert(dds.end(), texture.baseLevel.begin(), texture.baseLevel.end());
    return dds;
}

}

std::string Gta5Shader::Texture(std::string_view parameter) const
{
    const auto found = textures.find(JenkinsHash(parameter));
    return found != textures.end() ? found->second : std::string{};
}

std::optional<std::vector<float>> Gta5Shader::Constant(std::string_view parameter) const
{
    const auto found = constants.find(JenkinsHash(parameter));
    return found != constants.end() ? std::optional<std::vector<float>>(found->second) : std::nullopt;
}

namespace Gta5Resource
{

std::string_view ShaderName(std::uint32_t nameHash)
{
    static const std::unordered_map<std::uint32_t, std::string_view> names = []
    {
        static constexpr std::string_view kNames[] = {
            "default", "cloth_normal_spec", "vehicle_badges", "vehicle_basic", "vehicle_blurredrotor",
            "vehicle_blurredrotor_emissive", "vehicle_cloth", "vehicle_cloth2", "vehicle_cutout", "vehicle_dash_emissive",
            "vehicle_dash_emissive_opaque", "vehicle_decal", "vehicle_decal2", "vehicle_detail", "vehicle_detail2",
            "vehicle_emissive_alpha", "vehicle_emissive_opaque", "vehicle_generic", "vehicle_interior", "vehicle_interior2",
            "vehicle_licenseplate", "vehicle_lightsemissive", "vehicle_lightsemissive_siren", "vehicle_mesh",
            "vehicle_mesh_enveff", "vehicle_mesh2_enveff", "vehicle_paint1", "vehicle_paint1_enveff", "vehicle_paint2",
            "vehicle_paint2_enveff", "vehicle_paint3", "vehicle_paint3_enveff", "vehicle_paint3_lvr", "vehicle_paint4",
            "vehicle_paint4_emissive", "vehicle_paint4_enveff", "vehicle_paint5_enveff", "vehicle_paint6",
            "vehicle_paint6_enveff", "vehicle_paint7", "vehicle_paint7_enveff", "vehicle_paint8", "vehicle_paint9",
            "vehicle_shuts", "vehicle_tire", "vehicle_tire_emissive", "vehicle_track", "vehicle_track_ammo",
            "vehicle_track_emissive", "vehicle_track_siren", "vehicle_track2", "vehicle_track2_emissive",
            "vehicle_vehglass", "vehicle_vehglass_inner"};
        std::unordered_map<std::uint32_t, std::string_view> map;
        for (const std::string_view name : kNames)
        {
            map[JenkinsHash(name)] = name;
        }
        return map;
    }();
    const auto found = names.find(nameHash);
    return found != names.end() ? found->second : std::string_view{};
}

size_t BaseLevelSize(std::uint32_t format, std::uint32_t width, std::uint32_t height)
{
    const size_t blocks = static_cast<size_t>(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4);
    const size_t pixels = static_cast<size_t>(width) * height;
    switch (format)
    {
    case 70: case 71: case 72: // BC1
    case 79: case 80: case 81: // BC4
        return blocks * 8;
    case 73: case 74: case 75: // BC2
    case 76: case 77: case 78: // BC3
    case 82: case 83: case 84: // BC5
    case 97: case 98: case 99: // BC7
        return blocks * 16;
    case 27: case 28: case 29: // R8G8B8A8
    case 87: case 88: case 90: case 91: case 93: // B8G8R8A8 / X8
        return pixels * 4;
    case 85: case 86: // B5G6R5, B5G5R5A1
        return pixels * 2;
    case 61: case 65: // R8, A8
        return pixels;
    default:
        return 0;
    }
}

std::vector<Gta5Texture> ReadTextureDictionary(const RageResource& resource, std::uint64_t address)
{
    // rage::pgDictionary<grcTexture>: the name hashes at 0x20, the textures at 0x30.
    std::vector<Gta5Texture> textures;
    const ArrayHeader list = ReadArrayHeader(resource, address + 0x30);
    textures.reserve(list.count);
    for (std::uint16_t index = 0; index < list.count; ++index)
    {
        const std::uint64_t texture = resource.Read<std::uint64_t>(list.pointer + index * 8u);
        if (texture != 0)
        {
            textures.push_back(ReadTexture(resource, texture, true));
        }
    }
    return textures;
}

std::vector<Gta5Texture> LoadTextureDictionary(const std::filesystem::path& path)
{
    const RageResource resource = RageResource::Load(path);
    if (resource.Version() != kGta5Gen9TextureDictionaryVersion)
    {
        throw std::runtime_error("'" + path.string() + "' is a version " + std::to_string(resource.Version()) +
                                 " texture dictionary; only GTA V Enhanced's (version 5) is read");
    }
    return ReadTextureDictionary(resource, RageResource::kSystemBase);
}

Gta5Fragment ReadFragment(const RageResource& resource)
{
    // rage::fragType: the drawable at 0x30, the name at 0x58, the physics LODs at 0xF0.
    const std::uint64_t root = RageResource::kSystemBase;
    Gta5Fragment fragment;
    const std::uint64_t drawable = resource.Read<std::uint64_t>(root + 0x30);
    if (drawable == 0)
    {
        throw std::runtime_error("'" + resource.Source() + "' has no drawable");
    }
    fragment.name = resource.ReadString(resource.Read<std::uint64_t>(root + 0x58));
    fragment.drawable = ReadDrawable(resource, drawable);

    const std::uint64_t lodGroup = resource.Read<std::uint64_t>(root + 0xF0);
    const std::uint64_t lod = lodGroup != 0 ? resource.Read<std::uint64_t>(lodGroup + 0x10) : 0;
    if (lod != 0)
    {
        const std::uint64_t children = resource.Read<std::uint64_t>(lod + 0xD0);
        const std::uint8_t childCount = resource.Read<std::uint8_t>(lod + 0x11D);
        for (std::uint8_t index = 0; index < childCount; ++index)
        {
            const std::uint64_t child = resource.Read<std::uint64_t>(children + index * 8u);
            if (child == 0)
            {
                continue;
            }
            Gta5FragmentChild out;
            out.boneTag = resource.Read<std::uint16_t>(child + 0x12);
            const std::uint64_t childDrawable = resource.Read<std::uint64_t>(child + 0xA0);
            if (childDrawable != 0)
            {
                Gta5Drawable read = ReadDrawable(resource, childDrawable);
                const bool drawn = std::any_of(read.lods.begin(), read.lods.end(),
                                               [](const std::vector<Gta5Model>& models) { return !models.empty(); });
                if (drawn)
                {
                    out.drawable = std::move(read);
                }
            }
            fragment.children.push_back(std::move(out));
        }
    }
    return fragment;
}

Gta5Fragment LoadFragment(const std::filesystem::path& path)
{
    const RageResource resource = RageResource::Load(path);
    if (resource.Version() != kGta5Gen9FragmentVersion)
    {
        throw std::runtime_error("'" + path.string() + "' is a version " + std::to_string(resource.Version()) +
                                 " fragment; only GTA V Enhanced's (version 171) is read");
    }
    return ReadFragment(resource);
}

TextureData DecodeTexture(const Gta5Texture& texture)
{
    if (texture.baseLevel.empty())
    {
        throw std::runtime_error("GTA V texture '" + texture.name + "' has no texels in a format the reader knows (" +
                                 std::to_string(texture.format) + ")");
    }
    const std::vector<std::uint8_t> dds = BuildDds(texture);
    return DdsDecoder::Decode(dds.data(), dds.size(), texture.name);
}

}

}
