#include "gta5_importer.h"

#include "gta5_game_data.h"
#include "gta5_resource.h"

#include <engine/core/log/log.h>
#include <engine/core/text/ascii.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace me
{

namespace
{
using Json = nlohmann::json;

constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

// GTA V's normal maps keep X and Y in red and green and leave blue at 255; the engine reads a
// tangent-space normal from all three, so Z is rebuilt from X and Y.
constexpr bool kFlipNormalGreen = false;

// The colour sets a vehicle that carvariations.meta does not list (the base game's are in a binary
// carvariations.ymt) is offered in: palette indices for primary, secondary, pearl, wheels, trim, dash.
constexpr std::array<std::array<int, 6>, 6> kFallbackColorSets{{
    {111, 111, 0, 156, 0, 0}, // Metallic White
    {0, 0, 0, 156, 0, 0},     // Metallic Black
    {4, 4, 0, 156, 0, 0},     // Metallic Silver
    {27, 27, 0, 156, 0, 0},   // Metallic Red
    {64, 64, 0, 156, 0, 0},   // Metallic Blue
    {89, 89, 0, 156, 0, 0},   // Metallic Race Yellow
}};

void WriteFileBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)))
    {
        throw std::runtime_error("Cannot write '" + path.string() + "'");
    }
}

void AppendToVector(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

void WritePng(const std::filesystem::path& path, int width, int height, int channels, const std::uint8_t* pixels)
{
    std::vector<std::uint8_t> encoded;
    if (stbi_write_png_to_func(AppendToVector, &encoded, width, height, channels, pixels, width * channels) == 0)
    {
        throw std::runtime_error("Cannot encode '" + path.string() + "' as PNG");
    }
    WriteFileBytes(path, encoded.data(), encoded.size());
}

std::string SafeStem(const std::string& name)
{
    std::string stem = name;
    for (char& character : stem)
    {
        const unsigned char code = static_cast<unsigned char>(character);
        if (!(std::isalnum(code) || character == '_' || character == '-'))
        {
            character = '_';
        }
    }
    return stem.empty() ? std::string("texture") : stem;
}

float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float Round(float value, int digits)
{
    const float scale = std::pow(10.0f, static_cast<float>(digits));
    return std::round(value * scale) / scale;
}

Json Vec3Json(const glm::vec3& value)
{
    return Json::array({Round(value.x, 5), Round(value.y, 5), Round(value.z, 5)});
}

// ---- Textures -----------------------------------------------------------------------------------

// Every texture in reach of the vehicle, by lower-case name: its own first, then the dictionaries
// in the order the game looks in them, each read only once something is still missing.
class TextureLibrary
{
  public:
    TextureLibrary(std::vector<Gta5Texture> own, std::vector<std::filesystem::path> dictionaries) : m_pending(std::move(dictionaries))
    {
        Add(std::move(own));
    }

    const Gta5Texture* Find(const std::string& name)
    {
        const std::string key = ToLowerAscii(name);
        for (;;)
        {
            const auto found = m_textures.find(key);
            if (found != m_textures.end())
            {
                return &found->second;
            }
            if (m_next >= m_pending.size())
            {
                return nullptr;
            }
            const std::filesystem::path path = m_pending[m_next++];
            try
            {
                Add(Gta5Resource::LoadTextureDictionary(path));
            }
            catch (const std::exception& exception)
            {
                LOG_WARN("GTA V import: skipping texture dictionary '{}': {}", path.string(), exception.what());
            }
        }
    }

  private:
    void Add(std::vector<Gta5Texture> textures)
    {
        for (Gta5Texture& texture : textures)
        {
            if (!texture.baseLevel.empty())
            {
                m_textures.emplace(ToLowerAscii(texture.name), std::move(texture));
            }
        }
    }

    std::unordered_map<std::string, Gta5Texture> m_textures;
    std::vector<std::filesystem::path> m_pending;
    size_t m_next = 0;
};

enum class ImageKind
{
    Color,     // as it is
    Normal,    // Z rebuilt from X and Y
    Roughness, // a metallic-roughness map from a specular map's glossiness
    Livery,    // a livery's colour over the paint, by its alpha
};

struct ImageRequest
{
    ImageKind kind = ImageKind::Color;
    std::string texture;
    std::uint32_t paintRgb = 0; // Livery: the paint under it, 0xRRGGBB sRGB
    bool keepAlpha = false;     // Color: write the alpha channel
};

std::string RequestKey(const ImageRequest& request)
{
    return std::to_string(static_cast<int>(request.kind)) + "|" + request.texture + "|" + std::to_string(request.paintRgb) + "|" +
           (request.keepAlpha ? "a" : "");
}

struct ImageResult
{
    // The written file, "textures/<name>.png"; empty when the texture is uniform or missing.
    std::string uri;
    // A texture of one texel value everywhere: that value (linear colour, alpha) instead of a file.
    std::optional<glm::vec4> uniform;
    bool missing = false;
};

// ---- Materials ----------------------------------------------------------------------------------

struct PaintLook
{
    glm::vec3 color{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.05f;
};

// How a palette colour looks on a painted panel. GTA's palette holds the paint's diffuse colour; a
// metal finish's reflectance is brighter than that, so it is raised to a plausible metal's.
PaintLook LookOfPaint(int paletteIndex)
{
    const std::optional<Gta5PaletteColor> entry = Gta5PaletteEntry(paletteIndex);
    PaintLook look;
    look.color = Gta5PaletteLinear(paletteIndex);
    const Gta5PaintFinish finish = entry ? entry->finish : Gta5PaintFinish::Metallic;
    const auto asMetal = [&](float reflectance)
    {
        const float brightest = std::max({look.color.r, look.color.g, look.color.b, 1e-3f});
        look.color = glm::clamp(look.color * (reflectance / brightest), glm::vec3(0.0f), glm::vec3(1.0f));
    };
    switch (finish)
    {
    case Gta5PaintFinish::Metallic:
        look.metallic = 0.4f;
        look.roughness = 0.35f;
        look.clearcoat = 1.0f;
        look.clearcoatRoughness = 0.03f;
        break;
    case Gta5PaintFinish::Util:
        look.roughness = 0.4f;
        look.clearcoat = 1.0f;
        look.clearcoatRoughness = 0.05f;
        break;
    case Gta5PaintFinish::Matte:
        look.roughness = 0.65f;
        break;
    case Gta5PaintFinish::Worn:
        look.roughness = 0.6f;
        look.clearcoat = 0.3f;
        look.clearcoatRoughness = 0.3f;
        break;
    case Gta5PaintFinish::Brushed:
        look.metallic = 1.0f;
        look.roughness = 0.3f;
        asMetal(0.6f);
        break;
    case Gta5PaintFinish::Chrome:
        look.metallic = 1.0f;
        look.roughness = 0.04f;
        look.color = glm::vec3(0.9f);
        break;
    case Gta5PaintFinish::Gold:
        look.metallic = 1.0f;
        look.roughness = 0.15f;
        look.color = Gta5PaletteLinear(160);
        break;
    case Gta5PaintFinish::Satin:
        look.metallic = 1.0f;
        look.roughness = 0.4f;
        look.color = Gta5PaletteLinear(160);
        break;
    }
    return look;
}

bool StartsWith(const std::string& text, std::string_view prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

// What a shader instance becomes, before the textures are known.
struct MaterialPlan
{
    std::string name;
    std::string shader;
    std::string baseTexture;
    std::string liveryTexture;
    std::string normalTexture;
    float normalScale = 1.0f;
    std::string specularTexture;
    glm::vec3 tint{1.0f};
    int slot = 0;
    bool paint = false;
    bool glass = false;
    bool blend = false;
    bool mask = false;
    std::optional<float> exponent;
};

MaterialPlan PlanMaterial(const Gta5Shader& shader, size_t index)
{
    MaterialPlan plan;
    plan.shader = shader.name;
    plan.name = shader.name + "_" + std::to_string(index);
    const std::string& name = shader.name;
    plan.paint = StartsWith(name, "vehicle_paint");
    plan.glass = StartsWith(name, "vehicle_vehglass");
    if (name == "vehicle_licenseplate")
    {
        plan.baseTexture = shader.Texture("PlateBgTex");
        plan.normalTexture = shader.Texture("PlateBgBumpTex");
    }
    else
    {
        plan.baseTexture = shader.Texture("DiffuseTex");
        plan.normalTexture = shader.Texture("BumpTex");
        plan.specularTexture = shader.Texture("SpecularTex");
    }
    if (plan.paint)
    {
        plan.liveryTexture = shader.Texture("DiffuseTex2");
        plan.specularTexture.clear(); // the paint's finish decides its gloss
    }
    if (const std::optional<std::vector<float>> bumpiness = shader.Constant("Bumpiness"); bumpiness && !bumpiness->empty())
    {
        plan.normalScale = std::clamp((*bumpiness)[0], 0.0f, 4.0f);
    }
    if (const std::optional<std::vector<float>> specular = shader.Constant("Specular"); specular && !specular->empty())
    {
        plan.exponent = (*specular)[0];
    }
    const std::vector<float> diffuseColor = shader.Constant("DiffuseColor").value_or(std::vector<float>{});
    plan.slot = Gta5Importer::PaintSlot(diffuseColor);
    if (plan.slot == 0 && diffuseColor.size() >= 3 && diffuseColor[0] != 2.0f)
    {
        plan.tint = glm::clamp(glm::vec3(diffuseColor[0], diffuseColor[1], diffuseColor[2]), glm::vec3(0.0f), glm::vec3(1.0f));
    }
    // The render bucket: 0 opaque, 1 alpha, 2 decal, 3 cutout.
    plan.blend = plan.glass || shader.renderBucket == 1 || shader.renderBucket == 2;
    plan.mask = shader.renderBucket == 3;
    return plan;
}

// ---- Geometry -----------------------------------------------------------------------------------

struct Primitive
{
    size_t shaderIndex = 0;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
    std::array<std::vector<glm::vec2>, 2> texCoords;
    std::vector<std::uint32_t> indices;
};

struct NodeRecord
{
    std::string name;
    int parent = -1;
    // Relative to the parent, in GTA's frame below the root.
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::mat4 world{1.0f};
    std::vector<Primitive> primitives;
    std::vector<int> children;
};

// The triangles of `geometry` for which `keep(vertex)` holds of their first vertex, with the
// vertices they use transformed by `transform` (rotations and translations only).
Primitive ExtractPrimitive(const Gta5Geometry& geometry, const glm::mat4& transform, const std::function<bool(std::uint32_t)>& keep)
{
    Primitive primitive;
    primitive.shaderIndex = geometry.shaderIndex;
    const glm::mat3 rotation = glm::mat3(transform);
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    const bool hasNormals = geometry.normals.size() == geometry.positions.size();
    const bool hasTangents = geometry.tangents.size() == geometry.positions.size();
    for (size_t triangle = 0; triangle + 2 < geometry.indices.size(); triangle += 3)
    {
        if (!keep(geometry.indices[triangle]))
        {
            continue;
        }
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const std::uint32_t source = geometry.indices[triangle + corner];
            auto [it, inserted] = remap.emplace(source, static_cast<std::uint32_t>(primitive.positions.size()));
            if (inserted)
            {
                primitive.positions.push_back(glm::vec3(transform * glm::vec4(geometry.positions[source], 1.0f)));
                if (hasNormals)
                {
                    const glm::vec3 normal = rotation * geometry.normals[source];
                    const float length = glm::length(normal);
                    primitive.normals.push_back(length > 1e-6f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f));
                }
                if (hasTangents)
                {
                    const glm::vec4& tangent = geometry.tangents[source];
                    glm::vec3 direction = rotation * glm::vec3(tangent);
                    const float length = glm::length(direction);
                    direction = length > 1e-6f ? direction / length : glm::vec3(1.0f, 0.0f, 0.0f);
                    primitive.tangents.emplace_back(direction, tangent.w < 0.0f ? -1.0f : 1.0f);
                }
                for (size_t set = 0; set < primitive.texCoords.size(); ++set)
                {
                    if (geometry.texCoords[set].size() == geometry.positions.size())
                    {
                        primitive.texCoords[set].push_back(geometry.texCoords[set][source]);
                    }
                }
            }
            primitive.indices.push_back(it->second);
        }
    }
    return primitive;
}

glm::mat4 LocalMatrix(const NodeRecord& node)
{
    return glm::translate(glm::mat4(1.0f), node.translation) * glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.0f), node.scale);
}

bool IsWheelBone(const std::string& name)
{
    // wheel_lf, wheel_rr, wheel_lm1, ...: not wheelmesh_*.
    return StartsWith(name, "wheel_") && name.size() >= 8 && (name[6] == 'l' || name[6] == 'r');
}

// ---- The glTF document --------------------------------------------------------------------------

class GltfWriter
{
  public:
    size_t AddAccessor(const void* data, size_t count, size_t componentSize, int componentType, const char* type, size_t components,
                       int target, Json minimum = nullptr, Json maximum = nullptr)
    {
        while (m_binary.size() % 4 != 0)
        {
            m_binary.push_back(0);
        }
        const size_t offset = m_binary.size();
        const size_t length = count * componentSize * components;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_binary.insert(m_binary.end(), bytes, bytes + length);
        m_bufferViews.push_back(Json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}, {"target", target}});
        Json accessor{{"bufferView", m_bufferViews.size() - 1}, {"componentType", componentType}, {"count", count}, {"type", type}};
        if (!minimum.is_null())
        {
            accessor["min"] = std::move(minimum);
            accessor["max"] = std::move(maximum);
        }
        m_accessors.push_back(std::move(accessor));
        return m_accessors.size() - 1;
    }

    Json PrimitiveJson(const Primitive& primitive)
    {
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(std::numeric_limits<float>::lowest());
        for (const glm::vec3& position : primitive.positions)
        {
            low = glm::min(low, position);
            high = glm::max(high, position);
        }
        Json attributes = Json::object();
        attributes["POSITION"] = AddAccessor(primitive.positions.data(), primitive.positions.size(), 4, kFloat, "VEC3", 3, kArrayBuffer,
                                             Json::array({low.x, low.y, low.z}), Json::array({high.x, high.y, high.z}));
        if (!primitive.normals.empty())
        {
            attributes["NORMAL"] = AddAccessor(primitive.normals.data(), primitive.normals.size(), 4, kFloat, "VEC3", 3, kArrayBuffer);
        }
        if (!primitive.tangents.empty())
        {
            attributes["TANGENT"] = AddAccessor(primitive.tangents.data(), primitive.tangents.size(), 4, kFloat, "VEC4", 4, kArrayBuffer);
        }
        for (size_t set = 0; set < primitive.texCoords.size(); ++set)
        {
            if (!primitive.texCoords[set].empty())
            {
                attributes["TEXCOORD_" + std::to_string(set)] =
                    AddAccessor(primitive.texCoords[set].data(), primitive.texCoords[set].size(), 4, kFloat, "VEC2", 2, kArrayBuffer);
            }
        }
        size_t indices = 0;
        if (primitive.positions.size() <= 0xFFFF)
        {
            const std::vector<std::uint16_t> shortIndices(primitive.indices.begin(), primitive.indices.end());
            indices = AddAccessor(shortIndices.data(), shortIndices.size(), 2, kUnsignedShort, "SCALAR", 1, kElementArrayBuffer);
        }
        else
        {
            indices = AddAccessor(primitive.indices.data(), primitive.indices.size(), 4, kUnsignedInt, "SCALAR", 1, kElementArrayBuffer);
        }
        return Json{{"attributes", std::move(attributes)}, {"indices", indices}, {"mode", 4}};
    }

    std::vector<std::uint8_t>& Binary()
    {
        return m_binary;
    }
    Json& Accessors()
    {
        return m_accessors;
    }
    Json& BufferViews()
    {
        return m_bufferViews;
    }

  private:
    std::vector<std::uint8_t> m_binary;
    Json m_accessors = Json::array();
    Json m_bufferViews = Json::array();
};

// Decodes and writes the requested images on all cores; the results, by RequestKey.
std::map<std::string, ImageResult> WriteImages(const std::vector<ImageRequest>& requests, TextureLibrary& library,
                                               const std::filesystem::path& textureDirectory, const std::function<void(float)>& progress)
{
    // Which texture each request reads is settled on this thread (the library loads dictionaries).
    std::vector<const Gta5Texture*> sources;
    for (const ImageRequest& request : requests)
    {
        sources.push_back(library.Find(request.texture));
    }

    std::set<std::string> takenNames;
    std::vector<std::string> fileNames;
    for (const ImageRequest& request : requests)
    {
        std::string stem = SafeStem(request.texture);
        switch (request.kind)
        {
        case ImageKind::Normal:
            stem += "_normal";
            break;
        case ImageKind::Roughness:
            stem += "_roughness";
            break;
        case ImageKind::Livery:
        {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_on_%06x", request.paintRgb);
            stem += suffix;
            break;
        }
        case ImageKind::Color:
            break;
        }
        std::string name = stem + ".png";
        for (int counter = 2; takenNames.count(ToLowerAscii(name)) != 0; ++counter)
        {
            name = stem + "_" + std::to_string(counter) + ".png";
        }
        takenNames.insert(ToLowerAscii(name));
        fileNames.push_back(name);
    }

    std::vector<ImageResult> results(requests.size());
    std::atomic<size_t> next{0};
    std::atomic<size_t> finished{0};
    std::mutex progressMutex;
    const auto work = [&]
    {
        for (size_t index = next.fetch_add(1); index < requests.size(); index = next.fetch_add(1))
        {
            const ImageRequest& request = requests[index];
            ImageResult& result = results[index];
            if (sources[index] == nullptr)
            {
                result.missing = true;
            }
            else
            {
                TextureData image = Gta5Resource::DecodeTexture(*sources[index]);
                std::vector<std::uint8_t>& pixels = image.pixels;
                const size_t texels = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
                bool uniform = true;
                bool opaque = true;
                for (size_t texel = 0; texel < texels; ++texel)
                {
                    uniform = uniform && std::equal(pixels.begin() + texel * 4, pixels.begin() + texel * 4 + 4, pixels.begin());
                    opaque = opaque && pixels[texel * 4 + 3] == 255;
                }
                int channels = 3;
                std::vector<std::uint8_t> out(texels * 3);
                for (size_t texel = 0; texel < texels; ++texel)
                {
                    const std::uint8_t* in = pixels.data() + texel * 4;
                    std::uint8_t* to = out.data() + texel * 3;
                    switch (request.kind)
                    {
                    case ImageKind::Color:
                    case ImageKind::Livery:
                        to[0] = in[0];
                        to[1] = in[1];
                        to[2] = in[2];
                        if (request.kind == ImageKind::Livery)
                        {
                            const float alpha = in[3] / 255.0f;
                            for (int channel = 0; channel < 3; ++channel)
                            {
                                const float paint = static_cast<float>((request.paintRgb >> (16 - 8 * channel)) & 0xFF);
                                to[channel] = static_cast<std::uint8_t>(std::lround(paint + (in[channel] - paint) * alpha));
                            }
                        }
                        break;
                    case ImageKind::Normal:
                    {
                        const float x = in[0] / 127.5f - 1.0f;
                        const float y = in[1] / 127.5f - 1.0f;
                        const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                        to[0] = in[0];
                        to[1] = kFlipNormalGreen ? static_cast<std::uint8_t>(255 - in[1]) : in[1];
                        to[2] = static_cast<std::uint8_t>(std::lround((z * 0.5f + 0.5f) * 255.0f));
                        break;
                    }
                    case ImageKind::Roughness:
                        to[0] = 255;
                        to[1] = static_cast<std::uint8_t>(std::lround(Gta5Importer::GlossToRoughness(in[1] / 255.0f) * 255.0f));
                        to[2] = 0;
                        break;
                    }
                }
                if (uniform && request.kind != ImageKind::Livery)
                {
                    result.uniform = glm::vec4(SrgbToLinear(out[0] / 255.0f), SrgbToLinear(out[1] / 255.0f), SrgbToLinear(out[2] / 255.0f),
                                               pixels[3] / 255.0f);
                    if (request.kind != ImageKind::Color)
                    {
                        result.uniform = glm::vec4(out[0] / 255.0f, out[1] / 255.0f, out[2] / 255.0f, 1.0f);
                    }
                }
                else
                {
                    if (request.kind == ImageKind::Color && request.keepAlpha && !opaque)
                    {
                        channels = 4;
                        out.assign(pixels.begin(), pixels.end());
                    }
                    WritePng(textureDirectory / fileNames[index], image.width, image.height, channels, out.data());
                    result.uri = "textures/" + fileNames[index];
                }
            }
            if (progress)
            {
                const std::lock_guard<std::mutex> lock(progressMutex);
                progress(static_cast<float>(++finished) / static_cast<float>(requests.size()));
            }
        }
    };

    const size_t threadCount = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, std::max<size_t>(requests.size(), 1));
    std::vector<std::future<void>> helpers;
    for (size_t helper = 1; helper < threadCount; ++helper)
    {
        helpers.push_back(std::async(std::launch::async, work));
    }
    std::exception_ptr error;
    try
    {
        work();
    }
    catch (...)
    {
        error = std::current_exception();
        next.store(requests.size());
    }
    for (std::future<void>& helper : helpers)
    {
        try
        {
            helper.get();
        }
        catch (...)
        {
            if (!error)
            {
                error = std::current_exception();
            }
        }
    }
    if (error)
    {
        std::rethrow_exception(error);
    }

    std::map<std::string, ImageResult> byKey;
    for (size_t index = 0; index < requests.size(); ++index)
    {
        byKey[RequestKey(requests[index])] = results[index];
    }
    return byKey;
}

}

namespace Gta5Importer
{

bool IsGta5Path(const std::filesystem::path& path)
{
    return ToLowerAscii(path.extension().string()) == ".yft";
}

std::string ImportName(const std::filesystem::path& source)
{
    std::string stem = source.stem().string();
    if (stem.size() > 3 && ToLowerAscii(stem.substr(stem.size() - 3)) == "_hi")
    {
        stem.resize(stem.size() - 3);
    }
    return stem;
}

int PaintSlot(const std::vector<float>& diffuseColor)
{
    if (diffuseColor.size() < 2 || diffuseColor[0] != 2.0f)
    {
        return 0;
    }
    const int slot = static_cast<int>(std::lround(diffuseColor[1]));
    return slot >= 1 && slot <= 7 && slot != 5 ? slot : 0;
}

float GlossToRoughness(float gloss)
{
    return std::clamp(1.0f - 0.85f * std::clamp(gloss, 0.0f, 1.0f), 0.08f, 1.0f);
}

Gta5ImportReport ConvertToGltf(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory,
    const Gta5ImportOptions& options,
    const ImportProgressCallback& progress)
{
    float lastReported = 0.0f;
    const auto reportProgress = [&](float fraction)
    {
        lastReported = std::max(lastReported, std::clamp(fraction, 0.0f, 1.0f));
        if (progress)
        {
            progress(lastReported);
        }
    };

    const std::string name = ImportName(source);
    const std::filesystem::path folder = source.parent_path();
    const std::filesystem::path hiPath = folder / (name + "_hi.yft");
    const std::filesystem::path basePath = folder / (name + ".yft");
    std::error_code existsEc;
    const std::filesystem::path fragmentPath = std::filesystem::exists(hiPath, existsEc) ? hiPath : source;

    const std::filesystem::path gltfPath = targetDirectory / (name + ".gltf");
    const std::filesystem::path binaryPath = targetDirectory / "buffers" / (name + ".bin");
    if (std::filesystem::exists(gltfPath, existsEc) || std::filesystem::exists(binaryPath, existsEc))
    {
        throw std::runtime_error("'" + gltfPath.string() + "' already exists; an import does not overwrite it");
    }

    Gta5ImportReport report;
    report.fragmentPath = fragmentPath;
    const Gta5Fragment fragment = Gta5Resource::LoadFragment(fragmentPath);
    const Gta5Drawable& drawable = fragment.drawable;
    if (drawable.bones.empty() || drawable.lods[0].empty())
    {
        throw std::runtime_error("'" + fragmentPath.string() + "' has no skeleton or no detailed model");
    }
    reportProgress(0.05f);

    // The textures: the fragment's own (the _hi one's, then the plain one's), then its dictionaries.
    std::vector<Gta5Texture> ownTextures = drawable.textures;
    if (fragmentPath != basePath && std::filesystem::exists(basePath, existsEc))
    {
        try
        {
            const Gta5Fragment lowDetail = Gta5Resource::LoadFragment(basePath);
            ownTextures.insert(ownTextures.end(), lowDetail.drawable.textures.begin(), lowDetail.drawable.textures.end());
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("GTA V import: '{}' not read for its textures: {}", basePath.string(), exception.what());
        }
    }
    const std::shared_ptr<const Gta5GameData> gameData = Gta5GameData::ForFile(source);
    const std::optional<Gta5VehicleInfo> info = gameData->Vehicle(name);
    if (info)
    {
        report.gameName = info->gameName;
        report.makeName = info->makeName;
    }
    TextureLibrary library(std::move(ownTextures), gameData->TextureDictionaries(info && !info->txdName.empty() ? info->txdName : name, source));
    reportProgress(0.1f);

    // The colour sets, each a material variant.
    std::vector<Gta5ColorSet> colorSets = info ? info->colorSets : std::vector<Gta5ColorSet>{};
    if (colorSets.empty())
    {
        for (const std::array<int, 6>& indices : kFallbackColorSets)
        {
            colorSets.push_back(Gta5ColorSet{indices});
        }
    }

    // ---- Nodes: one per bone, in GTA's frame ----
    std::vector<NodeRecord> nodes;
    std::unordered_map<std::uint16_t, size_t> boneByTag;
    for (size_t index = 0; index < drawable.bones.size(); ++index)
    {
        const Gta5Bone& bone = drawable.bones[index];
        NodeRecord node;
        node.name = bone.name.empty() ? "bone_" + std::to_string(index) : bone.name;
        node.parent = bone.parent;
        node.translation = bone.translation;
        node.rotation = glm::normalize(bone.rotation);
        node.scale = bone.scale;
        node.world = (node.parent >= 0 ? nodes[static_cast<size_t>(node.parent)].world : glm::mat4(1.0f)) * LocalMatrix(node);
        if (node.parent >= 0)
        {
            nodes[static_cast<size_t>(node.parent)].children.push_back(static_cast<int>(index));
        }
        boneByTag.emplace(bone.tag, index);
        nodes.push_back(std::move(node));
    }
    const auto addNode = [&](int parent, const std::string& nodeName, const glm::quat& rotation) -> size_t
    {
        NodeRecord node;
        node.name = nodeName;
        node.parent = parent;
        node.rotation = rotation;
        node.world = nodes[static_cast<size_t>(parent)].world * LocalMatrix(node);
        nodes.push_back(std::move(node));
        nodes[static_cast<size_t>(parent)].children.push_back(static_cast<int>(nodes.size() - 1));
        return nodes.size() - 1;
    };

    // The node a bone's meshes hang under: the bone's own, except for the steering wheel, whose
    // meshes go under STEER_HR, the node the engine turns them about (its +Z along the column, which
    // is the bone's +Y).
    std::vector<size_t> meshNodeOfBone(nodes.size());
    for (size_t index = 0; index < meshNodeOfBone.size(); ++index)
    {
        meshNodeOfBone[index] = index;
        if (ToLowerAscii(nodes[index].name) == "steeringwheel")
        {
            meshNodeOfBone[index] = addNode(static_cast<int>(index), "STEER_HR", glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0)));
        }
    }
    const auto isExtra = [&](size_t bone) { return StartsWith(ToLowerAscii(nodes[bone].name), "extra_"); };

    // ---- The body: split per bone ----
    std::set<size_t> droppedExtras;
    for (const Gta5Model& model : drawable.lods[0])
    {
        for (const Gta5Geometry& geometry : model.geometries)
        {
            std::set<std::uint16_t> bones(geometry.bones.begin(), geometry.bones.end());
            for (const std::uint16_t bone : bones)
            {
                const size_t boneIndex = bone < nodes.size() ? bone : 0;
                if (!options.keepExtras && isExtra(boneIndex))
                {
                    droppedExtras.insert(boneIndex);
                    continue;
                }
                const size_t target = meshNodeOfBone[boneIndex];
                Primitive primitive = ExtractPrimitive(geometry, glm::inverse(nodes[target].world),
                                                       [&](std::uint32_t vertex) { return geometry.bones[vertex] == bone; });
                if (!primitive.indices.empty())
                {
                    nodes[target].primitives.push_back(std::move(primitive));
                }
            }
        }
    }
    report.droppedExtras = droppedExtras.size();

    // ---- The parts with drawables of their own: the wheels, and any other ----
    std::map<std::string, const Gta5Drawable*> partDrawables; // lower-case bone name -> drawable
    for (const Gta5FragmentChild& child : fragment.children)
    {
        const auto bone = boneByTag.find(child.boneTag);
        if (child.drawable && bone != boneByTag.end())
        {
            partDrawables.emplace(ToLowerAscii(nodes[bone->second].name), &*child.drawable);
        }
    }
    const auto attachDrawable = [&](size_t node, const Gta5Drawable& part)
    {
        const std::vector<Gta5Model>& models = !part.lods[0].empty() ? part.lods[0] : part.lods[1];
        for (const Gta5Model& model : models)
        {
            for (const Gta5Geometry& geometry : model.geometries)
            {
                Primitive primitive = ExtractPrimitive(geometry, glm::mat4(1.0f), [](std::uint32_t) { return true; });
                if (!primitive.indices.empty())
                {
                    nodes[node].primitives.push_back(std::move(primitive));
                }
            }
        }
    };
    const size_t boneNodeCount = drawable.bones.size();
    for (size_t index = 0; index < boneNodeCount; ++index)
    {
        const std::string boneName = ToLowerAscii(nodes[index].name);
        if (IsWheelBone(boneName))
        {
            // A corner without a wheel of its own draws the rear's (behind the front axle) or the
            // front's, half a turn round on the other side.
            const char side = boneName[6];
            const bool front = boneName.size() == 8 && boneName[7] == 'f';
            std::vector<std::pair<std::string, char>> candidates{{boneName, side}};
            if (!front)
            {
                candidates.push_back({"wheel_lr", 'l'});
                candidates.push_back({"wheel_rr", 'r'});
            }
            candidates.push_back({"wheel_lf", 'l'});
            candidates.push_back({"wheel_rf", 'r'});
            for (const auto& [candidate, candidateSide] : candidates)
            {
                const auto found = partDrawables.find(candidate);
                if (found == partDrawables.end())
                {
                    continue;
                }
                const glm::quat turn = candidateSide == side ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                                             : glm::angleAxis(glm::radians(180.0f), glm::vec3(0, 0, 1));
                attachDrawable(addNode(static_cast<int>(index), nodes[index].name + "_mesh", turn), *found->second);
                ++report.wheels;
                break;
            }
        }
        else if (const auto found = partDrawables.find(boneName); found != partDrawables.end())
        {
            attachDrawable(meshNodeOfBone[index], *found->second);
        }
    }
    reportProgress(0.2f);

    // ---- Materials and their images ----
    std::vector<MaterialPlan> plans;
    for (size_t index = 0; index < drawable.shaders.size(); ++index)
    {
        plans.push_back(PlanMaterial(drawable.shaders[index], index));
    }
    std::vector<bool> shaderUsed(plans.size(), false);
    for (const NodeRecord& node : nodes)
    {
        for (const Primitive& primitive : node.primitives)
        {
            if (primitive.shaderIndex < shaderUsed.size())
            {
                shaderUsed[primitive.shaderIndex] = true;
            }
        }
    }
    const auto slotIndex = [&](const MaterialPlan& plan, const Gta5ColorSet& set) -> std::optional<int>
    {
        static constexpr std::array<int, 8> kSlotToIndex{-1, 0, 1, 2, 3, -1, 4, 5};
        if (plan.slot <= 0 || plan.slot >= static_cast<int>(kSlotToIndex.size()) || kSlotToIndex[plan.slot] < 0)
        {
            return std::nullopt;
        }
        const int index = set.indices[static_cast<size_t>(kSlotToIndex[plan.slot])];
        // Trim and dashboard colour 0 (the default) leaves the texture as it is: tinted black, an
        // interior would be all but invisible.
        if ((plan.slot == 6 || plan.slot == 7) && index == 0)
        {
            return std::nullopt;
        }
        return index;
    };

    std::vector<ImageRequest> requests;
    std::set<std::string> requested;
    const auto request = [&](const ImageRequest& image)
    {
        if (!image.texture.empty() && requested.insert(RequestKey(image)).second)
        {
            requests.push_back(image);
        }
    };
    for (size_t index = 0; index < plans.size(); ++index)
    {
        if (!shaderUsed[index])
        {
            continue;
        }
        const MaterialPlan& plan = plans[index];
        if (!plan.liveryTexture.empty() && plan.slot != 0)
        {
            for (const Gta5ColorSet& set : colorSets)
            {
                const std::optional<int> paletteIndex = slotIndex(plan, set);
                const std::uint32_t rgb = paletteIndex ? Gta5PaletteEntry(*paletteIndex).value_or(Gta5PaletteColor{}).rgb : 0xFFFFFF;
                request(ImageRequest{ImageKind::Livery, plan.liveryTexture, rgb, false});
            }
        }
        else
        {
            request(ImageRequest{ImageKind::Color, plan.baseTexture, 0, plan.blend || plan.mask});
        }
        request(ImageRequest{ImageKind::Normal, plan.normalTexture, 0, false});
        request(ImageRequest{ImageKind::Roughness, plan.specularTexture, 0, false});
    }
    const std::filesystem::path textureDirectory = targetDirectory / "textures";
    for (const std::filesystem::path& directory : {targetDirectory, textureDirectory, binaryPath.parent_path()})
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
        {
            throw std::runtime_error("Cannot create '" + directory.string() + "': " + ec.message());
        }
    }
    const std::map<std::string, ImageResult> images =
        WriteImages(requests, library, textureDirectory, [&](float fraction) { reportProgress(0.2f + 0.6f * fraction); });
    std::set<std::string> missing;
    for (const ImageRequest& image : requests)
    {
        if (images.at(RequestKey(image)).missing)
        {
            missing.insert(image.texture);
        }
    }
    report.missingTextures.assign(missing.begin(), missing.end());

    Json gltfImages = Json::array();
    Json gltfTextures = Json::array();
    std::map<std::string, size_t> textureByUri;
    const auto textureIndex = [&](const std::string& uri) -> size_t
    {
        const auto found = textureByUri.find(uri);
        if (found != textureByUri.end())
        {
            return found->second;
        }
        gltfImages.push_back(Json{{"uri", uri}});
        gltfTextures.push_back(Json{{"source", gltfImages.size() - 1}, {"sampler", 0}});
        textureByUri[uri] = gltfTextures.size() - 1;
        return gltfTextures.size() - 1;
    };
    const auto result = [&](ImageKind kind, const std::string& texture, std::uint32_t paint, bool keepAlpha) -> const ImageResult*
    {
        if (texture.empty())
        {
            return nullptr;
        }
        const auto found = images.find(RequestKey(ImageRequest{kind, texture, paint, keepAlpha}));
        return found != images.end() && !found->second.missing ? &found->second : nullptr;
    };

    std::set<std::string> extensionsUsed;
    Json materials = Json::array();
    const auto buildMaterial = [&](const MaterialPlan& plan, const Gta5ColorSet& set, const std::string& suffix) -> Json
    {
        const std::optional<int> paletteIndex = slotIndex(plan, set);
        Json pbr = Json::object();
        glm::vec4 baseColor(plan.tint, 1.0f);
        float metallic = 0.0f;
        float roughness = plan.exponent ? Kn5Importer::SpecularExponentToRoughness(*plan.exponent) : 0.6f;
        std::optional<PaintLook> paint;
        if (paletteIndex)
        {
            paint = LookOfPaint(*paletteIndex);
        }

        if (!plan.liveryTexture.empty() && plan.slot != 0)
        {
            const std::uint32_t rgb = paletteIndex ? Gta5PaletteEntry(*paletteIndex).value_or(Gta5PaletteColor{}).rgb : 0xFFFFFF;
            if (const ImageResult* livery = result(ImageKind::Livery, plan.liveryTexture, rgb, false); livery && !livery->uri.empty())
            {
                pbr["baseColorTexture"] = {{"index", textureIndex(livery->uri)}, {"texCoord", 1}};
                paint.reset(); // the paint is in the texture
                if (paletteIndex)
                {
                    const PaintLook look = LookOfPaint(*paletteIndex);
                    metallic = look.metallic;
                    roughness = look.roughness;
                }
            }
        }
        else if (const ImageResult* base = result(ImageKind::Color, plan.baseTexture, 0, plan.blend || plan.mask))
        {
            if (base->uniform)
            {
                baseColor *= *base->uniform;
            }
            else
            {
                pbr["baseColorTexture"] = {{"index", textureIndex(base->uri)}};
            }
        }
        if (paint)
        {
            baseColor = glm::vec4(glm::vec3(baseColor) * paint->color, baseColor.a);
            if (plan.paint || plan.slot == 4)
            {
                metallic = paint->metallic;
                roughness = paint->roughness;
            }
        }
        if (plan.glass)
        {
            roughness = 0.05f;
        }

        if (const ImageResult* specular = result(ImageKind::Roughness, plan.specularTexture, 0, false))
        {
            if (specular->uniform)
            {
                roughness = specular->uniform->g;
            }
            else
            {
                pbr["metallicRoughnessTexture"] = {{"index", textureIndex(specular->uri)}};
                roughness = 1.0f;
            }
        }
        pbr["baseColorFactor"] = Json::array({Round(baseColor.r, 5), Round(baseColor.g, 5), Round(baseColor.b, 5), Round(baseColor.a, 5)});
        pbr["metallicFactor"] = Round(metallic, 4);
        pbr["roughnessFactor"] = Round(roughness, 4);

        Json material = Json::object();
        material["name"] = plan.name + suffix;
        material["pbrMetallicRoughness"] = std::move(pbr);
        material["extras"] = {{"gtaShader", plan.shader}};
        if (const ImageResult* normal = result(ImageKind::Normal, plan.normalTexture, 0, false); normal && !normal->uri.empty())
        {
            material["normalTexture"] = {{"index", textureIndex(normal->uri)}, {"scale", Round(plan.normalScale, 4)}};
        }
        if (paint && plan.paint && paint->clearcoat > 0.0f)
        {
            material["extensions"]["KHR_materials_clearcoat"] = {{"clearcoatFactor", paint->clearcoat},
                                                                 {"clearcoatRoughnessFactor", paint->clearcoatRoughness}};
            extensionsUsed.insert("KHR_materials_clearcoat");
        }
        if (plan.blend)
        {
            material["alphaMode"] = "BLEND";
        }
        else if (plan.mask)
        {
            material["alphaMode"] = "MASK";
            material["alphaCutoff"] = 0.5f;
        }
        return material;
    };

    // materialOf[shader][variant]
    std::vector<std::vector<size_t>> materialOf(plans.size());
    for (size_t index = 0; index < plans.size(); ++index)
    {
        if (!shaderUsed[index])
        {
            continue;
        }
        const MaterialPlan& plan = plans[index];
        std::map<std::string, size_t> distinct;
        for (size_t variant = 0; variant < colorSets.size(); ++variant)
        {
            Json material = buildMaterial(plan, colorSets[variant], "");
            const std::string key = material.dump();
            auto [it, inserted] = distinct.emplace(key, materials.size());
            if (inserted)
            {
                if (distinct.size() > 1)
                {
                    material["name"] = plan.name + "_colours_" + std::to_string(variant + 1);
                }
                materials.push_back(std::move(material));
            }
            materialOf[index].push_back(it->second);
        }
    }
    report.materials = materials.size();

    // ---- Meshes and nodes ----
    GltfWriter writer;
    Json meshes = Json::array();
    Json gltfNodes = Json::array();
    // The game's origin is the chassis's, near the axles; the model's is where its tyres touch the
    // ground (the lowest wheel vertex, else the lowest vertex), as an imported car stands on its floor.
    float groundZ = std::numeric_limits<float>::max();
    float lowestZ = std::numeric_limits<float>::max();
    for (const NodeRecord& node : nodes)
    {
        const bool wheel = node.name.size() > 5 && node.name.ends_with("_mesh") && IsWheelBone(ToLowerAscii(node.name));
        for (const Primitive& primitive : node.primitives)
        {
            for (const glm::vec3& position : primitive.positions)
            {
                const float z = (node.world * glm::vec4(position, 1.0f)).z;
                lowestZ = std::min(lowestZ, z);
                if (wheel)
                {
                    groundZ = std::min(groundZ, z);
                }
            }
        }
    }
    if (groundZ == std::numeric_limits<float>::max())
    {
        groundZ = lowestZ == std::numeric_limits<float>::max() ? 0.0f : lowestZ;
    }
    // Node 0 is the root; bone/record i is node i + 1. Its matrix takes GTA's (x, y, z) to glTF's
    // (x, z - ground, -y).
    gltfNodes.push_back(Json{{"name", name}, {"matrix", {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, Round(-groundZ, 5), 0, 1}}});
    for (const NodeRecord& node : nodes)
    {
        Json out{{"name", node.name}};
        if (node.translation != glm::vec3(0.0f))
        {
            out["translation"] = Vec3Json(node.translation);
        }
        if (node.rotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
        {
            out["rotation"] = Json::array({Round(node.rotation.x, 6), Round(node.rotation.y, 6), Round(node.rotation.z, 6), Round(node.rotation.w, 6)});
        }
        if (glm::any(glm::greaterThan(glm::abs(node.scale - glm::vec3(1.0f)), glm::vec3(1e-4f))))
        {
            out["scale"] = Vec3Json(node.scale);
        }
        if (!node.primitives.empty())
        {
            Json primitives = Json::array();
            for (const Primitive& primitive : node.primitives)
            {
                Json gltfPrimitive = writer.PrimitiveJson(primitive);
                const std::vector<size_t>& variants = primitive.shaderIndex < materialOf.size() ? materialOf[primitive.shaderIndex]
                                                                                                : std::vector<size_t>{};
                if (!variants.empty())
                {
                    gltfPrimitive["material"] = variants[0];
                    if (std::any_of(variants.begin(), variants.end(), [&](size_t material) { return material != variants[0]; }))
                    {
                        std::map<size_t, std::vector<size_t>> byMaterial;
                        for (size_t variant = 0; variant < variants.size(); ++variant)
                        {
                            byMaterial[variants[variant]].push_back(variant);
                        }
                        Json mappings = Json::array();
                        for (const auto& [material, variantIndices] : byMaterial)
                        {
                            mappings.push_back(Json{{"material", material}, {"variants", variantIndices}});
                        }
                        gltfPrimitive["extensions"]["KHR_materials_variants"] = {{"mappings", std::move(mappings)}};
                    }
                }
                report.triangles += primitive.indices.size() / 3;
                ++report.primitives;
                primitives.push_back(std::move(gltfPrimitive));
            }
            meshes.push_back(Json{{"name", node.name}, {"primitives", std::move(primitives)}});
            out["mesh"] = meshes.size() - 1;
            ++report.meshes;
        }
        gltfNodes.push_back(std::move(out));
    }
    Json rootChildren = Json::array();
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        if (nodes[index].parent < 0)
        {
            rootChildren.push_back(index + 1);
        }
        if (!nodes[index].children.empty())
        {
            Json children = Json::array();
            for (const int child : nodes[index].children)
            {
                children.push_back(child + 1);
            }
            gltfNodes[index + 1]["children"] = std::move(children);
        }
    }
    gltfNodes[0]["children"] = std::move(rootChildren);
    reportProgress(0.9f);

    // ---- The document ----
    Json document = Json::object();
    document["asset"] = {{"version", "2.0"}, {"generator", "MiniEngine GTA V importer"}};
    document["scene"] = 0;
    document["scenes"] = Json::array({Json{{"nodes", Json::array({0})}}});
    document["nodes"] = std::move(gltfNodes);
    document["meshes"] = std::move(meshes);
    document["materials"] = std::move(materials);
    document["accessors"] = std::move(writer.Accessors());
    document["bufferViews"] = std::move(writer.BufferViews());
    document["buffers"] = Json::array({Json{{"uri", "buffers/" + name + ".bin"}, {"byteLength", writer.Binary().size()}}});
    if (!gltfImages.empty())
    {
        document["images"] = std::move(gltfImages);
        document["textures"] = std::move(gltfTextures);
        document["samplers"] = Json::array({Json{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
    }
    Json variantNames = Json::array();
    for (size_t variant = 0; variant < colorSets.size(); ++variant)
    {
        const auto colorName = [](int index) { return std::string(Gta5PaletteEntry(index).value_or(Gta5PaletteColor{}).name); };
        const Gta5ColorSet& set = colorSets[variant];
        std::string label = std::to_string(variant + 1) + ": " + colorName(set.indices[0]);
        if (set.indices[1] != set.indices[0])
        {
            label += " / " + colorName(set.indices[1]);
        }
        variantNames.push_back(Json{{"name", label}});
    }
    if (variantNames.size() > 1)
    {
        document["extensions"]["KHR_materials_variants"] = {{"variants", std::move(variantNames)}};
        extensionsUsed.insert("KHR_materials_variants");
        report.colorVariants = colorSets.size();
    }
    document["extras"] = {{"gta5", {{"model", name}, {"gameName", report.gameName}, {"make", report.makeName}}}};
    if (!extensionsUsed.empty())
    {
        document["extensionsUsed"] = std::vector<std::string>(extensionsUsed.begin(), extensionsUsed.end());
    }

    WriteFileBytes(binaryPath, writer.Binary().data(), writer.Binary().size());
    const std::string text = document.dump();
    WriteFileBytes(gltfPath, text.data(), text.size());
    reportProgress(1.0f);

    report.gltfPath = gltfPath;
    report.nodes = nodes.size() + 1;
    report.images = textureByUri.size();
    LOG_INFO("GTA V '{}' ({} {}): {} nodes, {} meshes, {} primitives, {} triangles, {} wheels, {} materials, {} images, {} colour sets",
             fragmentPath.filename().string(), report.makeName, report.gameName, report.nodes, report.meshes, report.primitives,
             report.triangles, report.wheels, report.materials, report.images, colorSets.size());
    if (!report.missingTextures.empty())
    {
        std::string list;
        for (const std::string& texture : report.missingTextures)
        {
            list += (list.empty() ? "" : ", ") + texture;
        }
        LOG_WARN("GTA V '{}': {} textures not found in any dictionary in reach: {}", fragmentPath.filename().string(),
                 report.missingTextures.size(), list);
    }
    return report;
}

}

}
