#include "kn5_importer.h"

#include "dds_decoder.h"
#include "kn5_reader.h"
#include "texture_loader.h"

#include <engine/core/log/log.h>
#include <engine/core/text/ascii.h>

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace me
{

namespace
{
using Json = nlohmann::json;

constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;
// A texture blob smaller than this is a placeholder (or, encrypted, a decoy).
constexpr size_t kStubTextureBytes = 128;

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

bool EndsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsFinite(const float* values, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (!std::isfinite(values[index]))
        {
            return false;
        }
    }
    return true;
}

float Round(float value, int digits)
{
    const float scale = std::pow(10.0f, static_cast<float>(digits));
    return std::round(value * scale) / scale;
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error("Cannot create '" + path.string() + "'");
    }
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file)
    {
        throw std::runtime_error("Cannot write '" + path.string() + "'");
    }
}

// A texture blob as RGBA8, rows top-down: DDS through the engine's decoder, anything else
// (PNG, JPEG, TGA) through stb_image. nullopt, with a warning, when it cannot be decoded.
std::optional<TextureData> DecodeTextureBlob(const std::vector<std::uint8_t>& blob, const std::string& name)
{
    try
    {
        if (DdsDecoder::IsDds(blob.data(), blob.size()))
        {
            return DdsDecoder::Decode(blob.data(), blob.size(), name);
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            blob.data(), static_cast<int>(blob.size()), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr)
        {
            throw std::runtime_error(std::string("unrecognised image data (") + stbi_failure_reason() + ")");
        }
        TextureData image;
        image.width = width;
        image.height = height;
        image.channelCount = 4;
        image.pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        stbi_image_free(pixels);
        return image;
    }
    catch (const std::exception& error)
    {
        LOG_WARN("kn5 texture '{}' cannot be decoded: {}", name, error.what());
        return std::nullopt;
    }
}

void AppendToVector(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// Written through a memory buffer and std::ofstream so a non-ASCII path works on Windows too.
void WritePng(const std::filesystem::path& path, int width, int height, int channels, const std::uint8_t* pixels)
{
    std::vector<std::uint8_t> encoded;
    if (stbi_write_png_to_func(AppendToVector, &encoded, width, height, channels, pixels, width * channels) == 0)
    {
        throw std::runtime_error("Cannot encode '" + path.string() + "' as PNG");
    }
    WriteFileBytes(path, encoded.data(), encoded.size());
}

// A file name the texture can be written under: the kn5 name's stem with anything a filesystem
// or a URI might trip on replaced.
std::string SafeStem(const std::string& textureName)
{
    std::string stem = std::filesystem::path(textureName).stem().string();
    for (char& character : stem)
    {
        const unsigned char code = static_cast<unsigned char>(character);
        if (!(std::isalnum(code) || character == '_' || character == '-' || character == '.'))
        {
            character = '_';
        }
    }
    return stem.empty() ? std::string("texture") : stem;
}

// Assetto Corsa matches texture names ignoring case, so a kn5 can list "INT_DEcals.dds" and a
// material ask for "INT_Decals.dds". Entries that differ only in case collapse into the one with
// the largest blob (never the 1x1 placeholder), and every material slot is pointed at it.
// Returns how many entries were folded away.
size_t FoldTextureCase(Kn5Model& model)
{
    std::unordered_map<std::string, size_t> keep;
    for (size_t index = 0; index < model.textures.size(); ++index)
    {
        const std::string key = ToLowerAscii(model.textures[index].name);
        const auto found = keep.find(key);
        if (found == keep.end() || model.textures[index].data.size() > model.textures[found->second].data.size())
        {
            keep[key] = index;
        }
    }
    // Material slots are repointed even when nothing folded: one table entry can still differ in
    // case from the name a material asks for.
    const size_t folded = model.textures.size() - keep.size();
    std::unordered_map<std::string, std::string> canonical;
    std::vector<size_t> kept;
    for (const auto& [key, index] : keep)
    {
        canonical[key] = model.textures[index].name;
        kept.push_back(index);
    }
    std::sort(kept.begin(), kept.end());
    std::vector<Kn5Texture> textures;
    textures.reserve(kept.size());
    for (size_t index : kept)
    {
        textures.push_back(std::move(model.textures[index]));
    }
    model.textures = std::move(textures);

    for (Kn5Material& material : model.materials)
    {
        for (auto& [slot, name] : material.textures)
        {
            const auto found = canonical.find(ToLowerAscii(name));
            if (found != canonical.end())
            {
                name = found->second;
            }
        }
    }
    return folded;
}

std::optional<std::filesystem::path> ResolveSkinDirectory(const std::filesystem::path& kn5Path, const std::string& wanted)
{
    if (ToLowerAscii(wanted) == "none")
    {
        return std::nullopt;
    }
    const std::vector<std::string> skins = Kn5Importer::ListSkins(kn5Path);
    const std::filesystem::path root = kn5Path.parent_path() / "skins";
    if (wanted.empty())
    {
        return skins.empty() ? std::nullopt : std::optional<std::filesystem::path>(root / skins.front());
    }
    for (const std::string& skin : skins)
    {
        if (ToLowerAscii(skin) == ToLowerAscii(wanted))
        {
            return root / skin;
        }
    }
    std::string have;
    for (const std::string& skin : skins)
    {
        have += (have.empty() ? "" : ", ") + skin;
    }
    throw std::runtime_error(
        "No skin '" + wanted + "' in '" + root.string() + "'" + (have.empty() ? std::string(" (it has none)") : " - have: " + have));
}

// Overwrites kn5 texture blobs with the same-named files from the skin folder (ignoring case), as
// the game does at load time. Not only the albedo: a livery also ships *_MAP.dds and plate or
// badge sheets. Returns the names replaced.
std::unordered_set<std::string> ApplySkin(Kn5Model& model, const std::filesystem::path& skinDirectory)
{
    std::unordered_map<std::string, std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(skinDirectory, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc))
        {
            files[ToLowerAscii(it->path().filename().string())] = it->path();
        }
    }

    std::unordered_set<std::string> swapped;
    for (Kn5Texture& texture : model.textures)
    {
        const auto found = files.find(ToLowerAscii(texture.name));
        if (found == files.end())
        {
            continue;
        }
        try
        {
            texture.data = ReadFileBytes(found->second);
            swapped.insert(texture.name);
        }
        catch (const std::exception& error)
        {
            LOG_WARN("Skin texture '{}' was not applied: {}", found->second.string(), error.what());
        }
    }
    return swapped;
}

const Kn5Texture* FindTexture(const Kn5Model& model, const std::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }
    for (const Kn5Texture& texture : model.textures)
    {
        if (texture.name == name)
        {
            return &texture;
        }
    }
    return nullptr;
}

std::array<float, 3> Normalized(const std::array<float, 3>& value, const std::array<float, 3>& fallback)
{
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!std::isfinite(length) || length < 1e-8f)
    {
        return fallback;
    }
    return {value[0] / length, value[1] / length, value[2] / length};
}

// Any unit vector perpendicular to `normal`: a stand-in for a tangent the source lost.
std::array<float, 3> Perpendicular(const std::array<float, 3>& normal)
{
    const std::array<float, 3> axis =
        std::abs(normal[0]) < 0.9f ? std::array<float, 3>{1.0f, 0.0f, 0.0f} : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const std::array<float, 3> cross{
        axis[1] * normal[2] - axis[2] * normal[1],
        axis[2] * normal[0] - axis[0] * normal[2],
        axis[0] * normal[1] - axis[1] * normal[0]};
    return Normalized(cross, {1.0f, 0.0f, 0.0f});
}

class GltfBuilder
{
  public:
    GltfBuilder(const std::filesystem::path& textureDirectory, const Kn5ImportOptions& options)
        : m_textureDirectory(textureDirectory), m_options(options)
    {
    }

    Kn5ImportReport& Report()
    {
        return m_report;
    }

    // Records which textures a model's materials sample. Called for every model of the import
    // before any texture is written: a track's add-on kn5 samples textures its main kn5 carries.
    // Names are matched ignoring case, as the game does across all the models it loads.
    void CollectTextureUse(const Kn5Model& model)
    {
        // A diffuse keeps its alpha only where the material reads it (blended or alpha-tested):
        // elsewhere AC never samples it, and several cars carry a fully transparent one on opaque
        // paint and interior maps.
        for (const Kn5Material& material : model.materials)
        {
            for (const char* slot : {"txDiffuse", "txNormal"})
            {
                const std::string name = material.Texture(slot);
                if (name.empty() || (std::string(slot) == "txNormal" && IsDentMap(name)))
                {
                    continue;
                }
                m_usedTextures.insert(ToLowerAscii(name));
                if (material.alphaBlend || material.alphaTested)
                {
                    m_alphaTextures.insert(ToLowerAscii(name));
                }
            }
        }
    }

    // Writes the sampled textures this model carries and no earlier model already wrote: the game
    // keeps one texture per name.
    void WriteTextures(const Kn5Model& model)
    {
        for (const Kn5Texture& texture : model.textures)
        {
            const std::string key = ToLowerAscii(texture.name);
            if (!m_usedTextures.count(key) || texture.data.size() < kStubTextureBytes || m_textureUris.count(key) != 0)
            {
                continue;
            }
            std::optional<TextureData> image = DecodeTextureBlob(texture.data, texture.name);
            if (!image.has_value())
            {
                continue;
            }

            bool alpha = m_alphaTextures.count(key) != 0;
            if (alpha)
            {
                alpha = false;
                for (size_t index = 3; index < image->pixels.size(); index += 4)
                {
                    if (image->pixels[index] != 255)
                    {
                        alpha = true;
                        break;
                    }
                }
            }
            const std::string fileName = UniqueFileName(SafeStem(texture.name), ".png");
            const int channels = alpha ? 4 : 3;
            WritePng(m_textureDirectory / fileName, image->width, image->height, channels,
                     alpha ? image->pixels.data() : StripAlpha(*image).data());
            m_textureUris[key] = "textures/" + fileName;
        }
    }

    // Appends the model's materials; its meshes, emitted next, index them from here on.
    void AddMaterials(const Kn5Model& model)
    {
        m_materialBase = m_materials.size();
        m_materialCount = model.materials.size();
        for (const Kn5Material& material : model.materials)
        {
            m_materials.push_back(ConvertMaterial(model, material));
        }
        m_report.materials = m_materials.size();
    }

    // One kn5 node as one glTF node, children and all; nullopt when it is a dropped variant or a
    // childless mesh the game never draws.
    std::optional<size_t> Emit(const Kn5Node& node)
    {
        const bool hidden = node.HasGeometry() && (!node.renderable || Kn5Importer::IsTrackMarker(node.name));
        if (hidden)
        {
            ++m_report.hiddenMeshes;
            if (node.children.empty())
            {
                return std::nullopt;
            }
        }
        if (!m_options.keepVariants && (Kn5Importer::IsRuntimeVariant(node.name) || m_lowRes.count(node.name) != 0 ||
                                        (node.HasGeometry() && node.lodIn > 0.0f)))
        {
            ++m_report.droppedVariants;
            return std::nullopt;
        }

        Json gltfNode = Json::object();
        gltfNode["name"] = node.name;
        if (node.type == Kn5NodeType::Dummy)
        {
            ++m_report.transforms;
            if (node.matrix != kIdentity)
            {
                if (IsFinite(node.matrix.data(), node.matrix.size()))
                {
                    gltfNode["matrix"] = node.matrix;
                }
                else
                {
                    ++m_report.scrubbedMatrices;
                }
            }
        }
        else if (hidden)
        {
            // Kept only as the parent of its children.
        }
        else if (const std::optional<size_t> mesh = EmitMesh(node); mesh.has_value())
        {
            gltfNode["mesh"] = *mesh;
        }

        const size_t index = m_nodes.size();
        m_nodes.push_back(std::move(gltfNode));
        Json children = Json::array();
        for (const Kn5Node& child : node.children)
        {
            if (const std::optional<size_t> childIndex = Emit(child); childIndex.has_value())
            {
                children.push_back(*childIndex);
            }
        }
        if (!children.empty())
        {
            m_nodes[index]["children"] = std::move(children);
        }
        return index;
    }

    void SetLowResTwins(std::set<std::string> names)
    {
        m_lowRes = std::move(names);
    }

    // A layout model's placement, as a parent of its root node.
    size_t AddPlacement(const std::string& name, const std::array<float, 16>& matrix, size_t child)
    {
        m_nodes.push_back(Json{{"name", name}, {"matrix", matrix}, {"children", Json::array({child})}});
        return m_nodes.size() - 1;
    }

    size_t AddRoot(const std::string& name, const std::vector<size_t>& children)
    {
        // The half turn about Y that takes AC's axes (+X left, +Z forward) to glTF's (+X right,
        // -Z forward). Its determinant is +1, so winding and normals stay as they are.
        Json root = Json::object();
        root["name"] = name;
        root["matrix"] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
        if (!children.empty())
        {
            root["children"] = children;
        }
        m_nodes.push_back(std::move(root));
        return m_nodes.size() - 1;
    }

    Json BuildDocument(size_t rootNode, const std::string& binaryUri) const
    {
        Json document = Json::object();
        document["asset"] = {{"version", "2.0"}, {"generator", "MiniEngine kn5 importer (assetto-corsa-gltf rules)"}};
        document["scene"] = 0;
        document["scenes"] = Json::array({Json{{"nodes", Json::array({rootNode})}}});
        document["nodes"] = m_nodes;
        document["meshes"] = m_meshes;
        document["materials"] = m_materials;
        document["accessors"] = m_accessors;
        document["bufferViews"] = m_bufferViews;
        document["buffers"] = Json::array({Json{{"uri", binaryUri}, {"byteLength", m_binary.size()}}});
        if (!m_extensionsUsed.empty())
        {
            document["extensionsUsed"] = std::vector<std::string>(m_extensionsUsed.begin(), m_extensionsUsed.end());
        }
        if (!m_images.empty())
        {
            document["images"] = m_images;
            document["textures"] = m_textures;
            // Linear, trilinear mipmaps, repeat.
            document["samplers"] = Json::array({Json{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
        }
        return document;
    }

    const std::vector<std::uint8_t>& Binary() const
    {
        return m_binary;
    }

    size_t NodeCount() const
    {
        return m_nodes.size();
    }

    size_t ImageCount() const
    {
        return m_images.size();
    }

  private:
    static bool IsDentMap(const std::string& textureName)
    {
        // On AC's damage shaders txNormal holds the dent map, blended in with accumulated damage:
        // zero on an undamaged car. Bound as a normal map it caves every panel in.
        return ToLowerAscii(textureName).find("damage") != std::string::npos;
    }

    static std::vector<std::uint8_t> StripAlpha(const TextureData& image)
    {
        std::vector<std::uint8_t> rgb;
        rgb.reserve(image.pixels.size() / 4 * 3);
        for (size_t index = 0; index < image.pixels.size(); index += 4)
        {
            rgb.push_back(image.pixels[index]);
            rgb.push_back(image.pixels[index + 1]);
            rgb.push_back(image.pixels[index + 2]);
        }
        return rgb;
    }

    std::string UniqueFileName(const std::string& stem, const std::string& extension)
    {
        std::string candidate = stem + extension;
        for (int suffix = 1; m_fileNames.count(ToLowerAscii(candidate)) != 0 ||
                             std::filesystem::exists(m_textureDirectory / candidate);
             ++suffix)
        {
            candidate = stem + "_" + std::to_string(suffix) + extension;
        }
        m_fileNames.insert(ToLowerAscii(candidate));
        return candidate;
    }

    std::optional<size_t> TextureIndexForUri(const std::string& uri)
    {
        if (uri.empty())
        {
            return std::nullopt;
        }
        const auto found = m_textureIndices.find(uri);
        if (found != m_textureIndices.end())
        {
            return found->second;
        }
        m_images.push_back(Json{{"uri", uri}});
        m_textures.push_back(Json{{"source", m_images.size() - 1}, {"sampler", 0}});
        m_textureIndices[uri] = m_textures.size() - 1;
        return m_textures.size() - 1;
    }

    std::optional<size_t> TextureIndexForKn5(const std::string& textureName)
    {
        const auto found = m_textureUris.find(ToLowerAscii(textureName));
        return found == m_textureUris.end() ? std::nullopt : TextureIndexForUri(found->second);
    }

    // txDetail as a base-colour factor when it is a flat colour. On Kunos road cars this is where
    // the paint lives: the diffuse is a grey panel/AO template shared by every livery and the
    // shader multiplies the one-colour detail map over it.
    std::optional<std::array<float, 3>> DetailTint(const Kn5Model& model, const std::string& textureName)
    {
        const auto cached = m_tintCache.find(textureName);
        if (cached != m_tintCache.end())
        {
            return cached->second;
        }
        std::optional<std::array<float, 3>> tint;
        const Kn5Texture* texture = FindTexture(model, textureName);
        if (texture != nullptr && texture->data.size() >= kStubTextureBytes)
        {
            if (const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name))
            {
                tint = Kn5Importer::FlatDetailTint(image->pixels, image->width, image->height);
            }
        }
        m_tintCache[textureName] = tint;
        return tint;
    }

    // txMaps as a metallic-roughness image. Its red channel is the per-pixel specular intensity,
    // which scales the Blinn lobe the way the exponent does, so it folds into the same
    // exponent-to-roughness conversion; at full intensity it reproduces the constant exactly.
    // Metallic (blue) is 0: AC has no metallic workflow to map from.
    std::optional<std::string> BakeRoughness(const Kn5Model& model, const std::string& textureName, float exponent)
    {
        const std::string key = textureName + "|" + std::to_string(static_cast<int>(std::lround(exponent * 100.0f)));
        const auto cached = m_bakeCache.find(key);
        if (cached != m_bakeCache.end())
        {
            return cached->second;
        }
        m_bakeCache[key] = std::nullopt;
        const Kn5Texture* texture = FindTexture(model, textureName);
        if (texture == nullptr || texture->data.size() < kStubTextureBytes)
        {
            return std::nullopt;
        }
        const std::optional<TextureData> image = DecodeTextureBlob(texture->data, texture->name);
        if (!image.has_value())
        {
            return std::nullopt;
        }

        std::array<std::uint8_t, 256> lut{};
        for (int value = 0; value < 256; ++value)
        {
            const float intensity = std::max(static_cast<float>(value) / 255.0f, 0.02f);
            lut[value] = static_cast<std::uint8_t>(
                std::lround(Kn5Importer::SpecularExponentToRoughness(exponent * intensity) * 255.0f));
        }
        std::vector<std::uint8_t> rgb(static_cast<size_t>(image->width) * static_cast<size_t>(image->height) * 3, 0);
        for (size_t pixel = 0; pixel < rgb.size() / 3; ++pixel)
        {
            rgb[pixel * 3 + 1] = lut[image->pixels[pixel * 4]];
        }
        const std::string fileName = UniqueFileName(
            SafeStem(textureName) + "_rough" + std::to_string(static_cast<int>(exponent)), ".png");
        WritePng(m_textureDirectory / fileName, image->width, image->height, 3, rgb.data());
        m_bakeCache[key] = "textures/" + fileName;
        return m_bakeCache[key];
    }

    Json ConvertMaterial(const Kn5Model& model, const Kn5Material& material)
    {
        const float exponent = std::max(material.Property("ksSpecularEXP", 20.0f), 1.0f);
        // ksSpecular is the intensity: 0 means no highlight at all in AC (grass, trees).
        float specular = material.Property("ksSpecular", 1.0f);
        // ksMultilayer (track surfaces) drives its sheen from tarmacSpecularMultiplier instead,
        // where the author asked for a reflection at all (fresnelMaxLevel set).
        if (ToLowerAscii(material.shader).find("multilayer") != std::string::npos &&
            material.Property("fresnelMaxLevel", 0.0f) > 0.0f)
        {
            specular = std::max(specular, material.Property("tarmacSpecularMultiplier", specular));
        }
        const float effectiveExponent = exponent * std::max(specular, 0.02f);

        const bool useDetail = material.Property("useDetail", 0.0f) > 0.0f;
        const std::optional<std::array<float, 3>> tint =
            useDetail ? DetailTint(model, material.Texture("txDetail")) : std::nullopt;

        Json pbr = Json::object();
        pbr["baseColorFactor"] = tint.has_value() ? Json::array({(*tint)[0], (*tint)[1], (*tint)[2], 1.0f})
                                                  : Json::array({1.0f, 1.0f, 1.0f, 1.0f});
        pbr["metallicFactor"] = 0.0f;
        pbr["roughnessFactor"] = Round(Kn5Importer::SpecularExponentToRoughness(effectiveExponent), 4);
        if (const std::optional<size_t> diffuse = TextureIndexForKn5(material.Texture("txDiffuse")))
        {
            pbr["baseColorTexture"] = {{"index", *diffuse}};
        }
        if (const std::optional<std::string> baked =
                BakeRoughness(model, material.Texture("txMaps"), effectiveExponent))
        {
            // glTF multiplies factor and texture, so the per-pixel value takes over.
            pbr["metallicRoughnessTexture"] = {{"index", *TextureIndexForUri(*baked)}};
            pbr["roughnessFactor"] = 1.0f;
        }

        Json out = Json::object();
        out["name"] = material.name;
        out["pbrMetallicRoughness"] = std::move(pbr);
        out["doubleSided"] = false;

        // fresnelMaxLevel caps how much a surface reflects, which is what KHR_materials_specular's
        // factor is. Uncapped, near-black trim and glass render as nothing but sky.
        const float fresnelMax = material.Property("fresnelMaxLevel", 0.0f);
        if (fresnelMax > 0.0f)
        {
            out["extensions"]["KHR_materials_specular"] = {{"specularFactor", Round(std::min(fresnelMax, 1.0f), 4)}};
            m_extensionsUsed.insert("KHR_materials_specular");
        }

        // Car paint's second, much tighter lobe (sunSpecular / sunSpecularEXP) is the lacquer
        // over the base coat. Only painted panels set it.
        const float sunSpecular = material.Property("sunSpecular", 0.0f);
        if (sunSpecular > 0.0f)
        {
            const float sunExponent = std::max(material.Property("sunSpecularEXP", 1500.0f), 1.0f);
            const float clearcoatRoughness =
                std::clamp(std::sqrt(2.0f / (sunExponent + 2.0f)), 0.02f, 1.0f);
            out["extensions"]["KHR_materials_clearcoat"] = {
                {"clearcoatFactor", Round(std::min(sunSpecular / 20.0f, 1.0f), 4)},
                {"clearcoatRoughnessFactor", Round(clearcoatRoughness, 4)}};
            m_extensionsUsed.insert("KHR_materials_clearcoat");
        }

        const std::string normalName = material.Texture("txNormal");
        if (!IsDentMap(normalName))
        {
            if (const std::optional<size_t> normal = TextureIndexForKn5(normalName))
            {
                out["normalTexture"] = {{"index", *normal}};
            }
        }

        if (material.alphaBlend)
        {
            out["alphaMode"] = "BLEND";
        }
        else if (material.alphaTested)
        {
            out["alphaMode"] = "MASK";
            // ksAlphaRef is often 0 (or 0.01), and a MASK cutoff there passes every fragment.
            const float reference = material.Property("ksAlphaRef", 0.0f);
            out["alphaCutoff"] = reference >= 0.02f ? reference : 0.5f;
        }

        const float emissive = material.Property("ksEmissive", 0.0f);
        if (emissive > 0.0f)
        {
            const float level = std::min(emissive, 1.0f);
            out["emissiveFactor"] = {level, level, level};
        }
        return out;
    }

    size_t AppendView(const void* data, size_t size, int target)
    {
        while (m_binary.size() % 4 != 0)
        {
            m_binary.push_back(0);
        }
        const size_t offset = m_binary.size();
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_binary.insert(m_binary.end(), bytes, bytes + size);
        m_bufferViews.push_back(Json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}, {"target", target}});
        return m_bufferViews.size() - 1;
    }

    size_t AppendFloats(const std::vector<float>& values, size_t components, bool bounds)
    {
        const size_t view = AppendView(values.data(), values.size() * sizeof(float), kArrayBuffer);
        static const char* const kTypes[] = {"", "SCALAR", "VEC2", "VEC3", "VEC4"};
        Json accessor{{"bufferView", view}, {"componentType", kFloat}, {"count", values.size() / components}, {"type", kTypes[components]}};
        if (bounds)
        {
            std::vector<float> minimum(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(components));
            std::vector<float> maximum = minimum;
            for (size_t index = 0; index < values.size(); ++index)
            {
                minimum[index % components] = std::min(minimum[index % components], values[index]);
                maximum[index % components] = std::max(maximum[index % components], values[index]);
            }
            accessor["min"] = minimum;
            accessor["max"] = maximum;
        }
        m_accessors.push_back(std::move(accessor));
        return m_accessors.size() - 1;
    }

    std::optional<size_t> EmitMesh(const Kn5Node& node)
    {
        const size_t vertexCount = node.vertices.size();
        const size_t indexCount = node.indices.size() - node.indices.size() % 3;
        bool indicesValid = indexCount > 0 && vertexCount > 0;
        for (size_t index = 0; indicesValid && index < indexCount; ++index)
        {
            indicesValid = node.indices[index] < vertexCount;
        }
        if (!indicesValid)
        {
            if (indexCount > 0 && vertexCount > 0)
            {
                LOG_WARN("kn5 mesh '{}' indexes past its {} vertices; kept as an empty node", node.name, vertexCount);
            }
            ++m_report.emptyMeshes;
            return std::nullopt;
        }

        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<float> uvs;
        std::vector<float> tangents;
        positions.reserve(vertexCount * 3);
        normals.reserve(vertexCount * 3);
        uvs.reserve(vertexCount * 2);
        tangents.reserve(vertexCount * 4);
        for (const Kn5Vertex& vertex : node.vertices)
        {
            // Real files carry NaN (dash-light tangents, whole attributes): replace, and count.
            std::array<float, 3> position = vertex.position;
            if (!IsFinite(position.data(), 3))
            {
                position = {0.0f, 0.0f, 0.0f};
                ++m_report.scrubbedAttributes;
            }
            if (!IsFinite(vertex.normal.data(), 3))
            {
                ++m_report.scrubbedAttributes;
            }
            const std::array<float, 3> normal = Normalized(vertex.normal, {0.0f, 1.0f, 0.0f});
            std::array<float, 2> uv = vertex.uv;
            if (!IsFinite(uv.data(), 2))
            {
                uv = {0.0f, 0.0f};
                ++m_report.scrubbedAttributes;
            }
            if (m_options.flipUv)
            {
                uv[1] = -uv[1];
            }
            if (!IsFinite(vertex.tangent.data(), 3))
            {
                ++m_report.scrubbedAttributes;
            }
            const std::array<float, 3> tangent = Normalized(vertex.tangent, Perpendicular(normal));

            positions.insert(positions.end(), position.begin(), position.end());
            normals.insert(normals.end(), normal.begin(), normal.end());
            uvs.insert(uvs.end(), uv.begin(), uv.end());
            // kn5 tangents are vec3; the handedness it does not record is taken as +1.
            tangents.insert(tangents.end(), {tangent[0], tangent[1], tangent[2], 1.0f});
        }

        Json attributes = Json::object();
        attributes["POSITION"] = AppendFloats(positions, 3, true);
        attributes["NORMAL"] = AppendFloats(normals, 3, false);
        attributes["TEXCOORD_0"] = AppendFloats(uvs, 2, false);
        attributes["TANGENT"] = AppendFloats(tangents, 4, false);

        const size_t indexView = AppendView(node.indices.data(), indexCount * sizeof(std::uint16_t), kElementArrayBuffer);
        m_accessors.push_back(Json{{"bufferView", indexView}, {"componentType", kUnsignedShort}, {"count", indexCount}, {"type", "SCALAR"}});

        Json primitive{{"attributes", std::move(attributes)}, {"indices", m_accessors.size() - 1}};
        // A mesh's material index is relative to its own kn5.
        if (node.materialIndex < m_materialCount)
        {
            primitive["material"] = m_materialBase + node.materialIndex;
        }
        m_meshes.push_back(Json{{"name", node.name}, {"primitives", Json::array({std::move(primitive)})}});
        ++m_report.meshes;
        m_report.triangles += indexCount / 3;
        return m_meshes.size() - 1;
    }

    std::filesystem::path m_textureDirectory;
    Kn5ImportOptions m_options;
    Kn5ImportReport m_report;
    std::set<std::string> m_lowRes;
    size_t m_materialBase = 0;
    size_t m_materialCount = 0;
    std::vector<std::uint8_t> m_binary;
    Json m_nodes = Json::array();
    Json m_meshes = Json::array();
    Json m_materials = Json::array();
    Json m_accessors = Json::array();
    Json m_bufferViews = Json::array();
    Json m_images = Json::array();
    Json m_textures = Json::array();
    std::set<std::string> m_extensionsUsed;
    std::unordered_set<std::string> m_fileNames;
    // Keyed by lower-case texture name.
    std::unordered_set<std::string> m_usedTextures;
    std::unordered_set<std::string> m_alphaTextures;
    std::unordered_map<std::string, std::string> m_textureUris;
    std::unordered_map<std::string, size_t> m_textureIndices;
    std::unordered_map<std::string, std::optional<std::array<float, 3>>> m_tintCache;
    std::unordered_map<std::string, std::optional<std::string>> m_bakeCache;
};

void CollectNodeNames(const Kn5Node& node, std::vector<std::string>& names)
{
    names.push_back(node.name);
    for (const Kn5Node& child : node.children)
    {
        CollectNodeNames(child, names);
    }
}

// The colour a texture is everywhere, as each channel's middle in [0, 1]: nullopt when any channel
// spans more than 6 levels. Extrema over every texel, not a downsample: averaging first would
// flatten leather grain and brushed metal to a constant too.
std::optional<std::array<float, 3>> FlatColorMiddle(const std::vector<std::uint8_t>& rgba, int width, int height)
{
    const size_t texels = static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0));
    if (texels == 0 || rgba.size() < texels * 4)
    {
        return std::nullopt;
    }
    std::array<int, 3> low{255, 255, 255};
    std::array<int, 3> high{0, 0, 0};
    for (size_t texel = 0; texel < texels; ++texel)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const int value = rgba[texel * 4 + static_cast<size_t>(channel)];
            low[channel] = std::min(low[channel], value);
            high[channel] = std::max(high[channel], value);
        }
    }
    std::array<float, 3> middle{};
    for (int channel = 0; channel < 3; ++channel)
    {
        if (high[channel] - low[channel] > 6)
        {
            return std::nullopt;
        }
        middle[channel] = static_cast<float>(low[channel] + high[channel]) * 0.5f / 255.0f;
    }
    return middle;
}

// Meshes, triangles (in total and per material) and the subtrees a default import drops.
void SurveyNodes(
    const Kn5Node& node,
    const std::set<std::string>& lowRes,
    bool insideDropped,
    Kn5ModelSummary& summary,
    std::vector<size_t>& materialTriangles)
{
    bool dropped = insideDropped;
    if (!insideDropped && (Kn5Importer::IsRuntimeVariant(node.name) || lowRes.count(node.name) != 0 ||
                           (node.HasGeometry() && node.lodIn > 0.0f)))
    {
        ++summary.runtimeVariants;
        dropped = true;
    }
    if (node.HasGeometry() && (!node.renderable || Kn5Importer::IsTrackMarker(node.name)))
    {
        ++summary.hiddenMeshes;
    }
    else if (node.HasGeometry())
    {
        ++summary.meshes;
        summary.triangles += node.triangleCount;
        if (node.materialIndex < materialTriangles.size())
        {
            materialTriangles[node.materialIndex] += node.triangleCount;
        }
    }
    for (const Kn5Node& child : node.children)
    {
        SurveyNodes(child, lowRes, dropped, summary, materialTriangles);
    }
}
}

namespace Kn5Importer
{
bool IsKn5Path(const std::filesystem::path& path)
{
    return ToLowerAscii(path.extension().string()) == ".kn5";
}

std::vector<std::string> ListSkins(const std::filesystem::path& kn5Path)
{
    std::vector<std::string> skins;
    const std::filesystem::path root = kn5Path.parent_path() / "skins";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code dirEc;
        if (it->is_directory(dirEc))
        {
            skins.push_back(it->path().filename().string());
        }
    }
    std::sort(skins.begin(), skins.end());
    return skins;
}

bool IsRuntimeVariant(const std::string& nodeName)
{
    const std::string lower = ToLowerAscii(nodeName);
    return lower.find("blur") != std::string::npos || lower.find("damage") != std::string::npos;
}

bool IsTrackMarker(const std::string& nodeName)
{
    const std::string lower = ToLowerAscii(nodeName);
    const auto digitsFrom = [&lower](size_t start, size_t end)
    {
        return end > start && std::all_of(lower.begin() + static_cast<std::ptrdiff_t>(start),
                                          lower.begin() + static_cast<std::ptrdiff_t>(end),
                                          [](char c) { return c >= '0' && c <= '9'; });
    };
    for (const std::string_view prefix : {"ac_start_", "ac_pit_", "ac_hotlap_start_"})
    {
        if (lower.starts_with(prefix) && digitsFrom(prefix.size(), lower.size()))
        {
            return true;
        }
    }
    constexpr std::string_view kTime = "ac_time_";
    return lower.starts_with(kTime) && lower.size() > kTime.size() + 2 &&
           (lower.ends_with("_l") || lower.ends_with("_r")) && digitsFrom(kTime.size(), lower.size() - 2);
}

std::set<std::string> LowResTwins(const std::vector<std::string>& nodeNames)
{
    std::unordered_set<std::string> present;
    for (const std::string& name : nodeNames)
    {
        present.insert(ToLowerAscii(name));
    }
    std::set<std::string> twins;
    for (const std::string& name : nodeNames)
    {
        const std::string lower = ToLowerAscii(name);
        if (EndsWith(lower, "_lr") && present.count(lower.substr(0, lower.size() - 3) + "_hr") != 0)
        {
            twins.insert(name);
        }
    }
    return twins;
}

float SpecularExponentToRoughness(float exponent)
{
    return std::clamp(std::sqrt(2.0f / (exponent + 2.0f)), 0.04f, 1.0f);
}

std::optional<std::array<float, 3>> FlatDetailTint(const std::vector<std::uint8_t>& rgba, int width, int height)
{
    const std::optional<std::array<float, 3>> middle = FlatColorMiddle(rgba, width, height);
    if (!middle.has_value())
    {
        return std::nullopt;
    }
    std::array<float, 3> linear{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        // Doubled in gamma space, then linearised: the order is AC's, and on a mid-grey paint
        // the two orders differ by a stop and a half.
        const float doubled = std::min(2.0f * (*middle)[channel], 1.0f);
        linear[channel] = Round(
            doubled <= 0.04045f ? doubled / 12.92f : std::pow((doubled + 0.055f) / 1.055f, 2.4f), 5);
    }
    return linear;
}

std::vector<size_t> RankPaintedMaterials(
    const std::vector<std::string>& materialNames,
    const std::vector<bool>& painted,
    const std::vector<size_t>& triangles)
{
    // What a car is painted beside, which is the half that matters: a rim is a painted material
    // on most Kunos cars and can carry more triangles than the bodywork.
    static const std::regex kNotBody(
        "rim|wheel|tyre|tire|brake|calip|disc|glass|window|light|lamp|badge|logo|plate|mirror|seat|"
        "interior|cockpit|dash|carpet|leather|belt|driver|steer|engine|exhaust|grill|plastic|chrome|"
        "rubber|shadow");
    static const std::regex kPaintHint("car[_ ]?paint|(^|[^a-z])body([^a-z]|$)|chassis");

    struct Ranked
    {
        size_t index;
        int band;
        int inside;
        size_t triangles;
    };
    std::vector<Ranked> ranked;
    for (size_t index = 0; index < materialNames.size() && index < painted.size(); ++index)
    {
        if (!painted[index])
        {
            continue;
        }
        const std::string lower = ToLowerAscii(materialNames[index]);
        // An interior copy of the paint (INT_OCC_Carpaint) is the same colour inside the panels.
        const int inside = lower.rfind("int", 0) == 0 ? 1 : 0;
        const int band = std::regex_search(lower, kNotBody) ? 2 : (std::regex_search(lower, kPaintHint) ? 0 : 1);
        ranked.push_back({index, band, inside, index < triangles.size() ? triangles[index] : 0});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b)
                     {
                         if (a.band != b.band)
                         {
                             return a.band < b.band;
                         }
                         if (a.inside != b.inside)
                         {
                             return a.inside < b.inside;
                         }
                         return a.triangles > b.triangles;
                     });
    std::vector<size_t> order;
    order.reserve(ranked.size());
    for (const Ranked& entry : ranked)
    {
        order.push_back(entry.index);
    }
    return order;
}

}

namespace
{
// AC's ini: [SECTION] then KEY=VALUE, keys upper-cased. Hand-rolled because the files have
// duplicate keys, empty values and '%' in values.
std::vector<std::pair<std::string, std::map<std::string, std::string>>> ReadAcIni(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    const auto trim = [](std::string text)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        const size_t last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> sections;
    std::string line;
    while (std::getline(file, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line.rfind("//", 0) == 0)
        {
            continue;
        }
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            sections.emplace_back(line.substr(1, close == std::string::npos ? std::string::npos : close - 1), std::map<std::string, std::string>{});
            continue;
        }
        const size_t equals = line.find('=');
        if (!sections.empty() && equals != std::string::npos)
        {
            std::string key = trim(line.substr(0, equals));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
                           {
                               return static_cast<char>(std::toupper(character));
                           });
            sections.back().second[key] = trim(line.substr(equals + 1));
        }
    }
    return sections;
}

// "x, y, z" as three floats; zero for a missing or malformed value, as the game reads it.
std::array<float, 3> ParseTriple(const std::map<std::string, std::string>& values, const std::string& key)
{
    const auto found = values.find(key);
    if (found == values.end())
    {
        return {0.0f, 0.0f, 0.0f};
    }
    std::array<float, 3> triple{0.0f, 0.0f, 0.0f};
    std::stringstream stream(found->second);
    std::string part;
    for (size_t index = 0; index < 3 && std::getline(stream, part, ','); ++index)
    {
        try
        {
            size_t used = 0;
            triple[index] = std::stof(part, &used);
            if (part.find_first_not_of(" \t", used) != std::string::npos || !std::isfinite(triple[index]))
            {
                return {0.0f, 0.0f, 0.0f};
            }
        }
        catch (const std::exception&)
        {
            return {0.0f, 0.0f, 0.0f};
        }
    }
    return triple;
}

// The models an import converts: the kn5 itself, or a layout's.
std::vector<Kn5LayoutModel> ImportSources(const std::filesystem::path& source)
{
    if (Kn5Importer::IsLayoutPath(source))
    {
        return Kn5Importer::ReadLayout(source);
    }
    return {Kn5LayoutModel{source, {}, {}}};
}

// Counts one kn5's meshes, materials, textures and droppable variants into `summary`. Returns the
// triangles per material, for ranking its paint.
std::vector<size_t> SurveyModel(const Kn5Model& model, Kn5ModelSummary& summary)
{
    summary.materials += model.materials.size();
    summary.textures += model.textures.size();
    ++summary.models;
    std::vector<std::string> names;
    CollectNodeNames(model.root, names);
    std::vector<size_t> materialTriangles(model.materials.size(), 0);
    SurveyNodes(model.root, Kn5Importer::LowResTwins(names), false, summary, materialTriangles);
    return materialTriangles;
}

// Each livery beside a car, with the colour it paints the bodywork, then the embedded textures.
void SurveySkins(const std::filesystem::path& kn5Path, const Kn5Model& model, const std::vector<size_t>& materialTriangles, Kn5ModelSummary& summary)
{
    std::vector<std::string> materialNames;
    std::vector<bool> painted;
    for (const Kn5Material& material : model.materials)
    {
        materialNames.push_back(material.name);
        painted.push_back(!material.Texture("txDetail").empty() && material.Property("useDetail", 0.0f) > 0.0f);
    }
    const std::vector<size_t> paintOrder = Kn5Importer::RankPaintedMaterials(materialNames, painted, materialTriangles);

    // Keyed by the file the colour comes from: several materials (and every livery that does not
    // ship the texture) share one detail map, so each is decoded once.
    std::unordered_map<std::string, std::optional<std::array<std::uint8_t, 3>>> colors;
    const auto colorOf = [&](const std::string& key, const std::function<std::vector<std::uint8_t>()>& read)
    {
        const auto cached = colors.find(key);
        if (cached != colors.end())
        {
            return cached->second;
        }
        std::optional<std::array<std::uint8_t, 3>> color;
        const std::vector<std::uint8_t> blob = read();
        if (blob.size() >= kStubTextureBytes)
        {
            if (const std::optional<TextureData> image = DecodeTextureBlob(blob, key))
            {
                if (const std::optional<std::array<float, 3>> middle =
                        FlatColorMiddle(image->pixels, image->width, image->height))
                {
                    color = std::array<std::uint8_t, 3>{
                        static_cast<std::uint8_t>(std::lround((*middle)[0] * 255.0f)),
                        static_cast<std::uint8_t>(std::lround((*middle)[1] * 255.0f)),
                        static_cast<std::uint8_t>(std::lround((*middle)[2] * 255.0f))};
                }
            }
        }
        colors[key] = color;
        return color;
    };

    std::vector<std::string> skinNames = Kn5Importer::ListSkins(kn5Path);
    skinNames.push_back(std::string());
    for (const std::string& skinName : skinNames)
    {
        Kn5SkinSummary skin;
        skin.name = skinName;
        std::unordered_map<std::string, std::filesystem::path> overrides;
        if (!skinName.empty())
        {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(kn5Path.parent_path() / "skins" / skinName, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code fileEc;
                if (it->is_regular_file(fileEc))
                {
                    overrides[ToLowerAscii(it->path().filename().string())] = it->path();
                }
            }
        }
        for (size_t materialIndex : paintOrder)
        {
            const std::string detail = model.materials[materialIndex].Texture("txDetail");
            const auto own = overrides.find(ToLowerAscii(detail));
            std::optional<std::array<std::uint8_t, 3>> color;
            const bool fromSkin = own != overrides.end();
            if (fromSkin)
            {
                const std::filesystem::path file = own->second;
                color = colorOf(file.string(), [&]()
                                {
                                    try
                                    {
                                        return ReadFileBytes(file);
                                    }
                                    catch (const std::exception&)
                                    {
                                        return std::vector<std::uint8_t>();
                                    }
                                });
            }
            else
            {
                color = colorOf("kn5:" + detail, [&]()
                                {
                                    const Kn5Texture* texture = FindTexture(model, detail);
                                    return texture != nullptr ? texture->data : std::vector<std::uint8_t>();
                                });
            }
            if (color.has_value())
            {
                // The first flat colour in body-first order is the car's colour.
                skin.paint = color;
                skin.paintMaterial = model.materials[materialIndex].name;
                skin.paintFromSkin = fromSkin;
                break;
            }
        }
        summary.skins.push_back(std::move(skin));
    }
}
}

namespace Kn5Importer
{
bool IsLayoutPath(const std::filesystem::path& path)
{
    const std::string name = ToLowerAscii(path.filename().string());
    return name == "models.ini" || (name.rfind("models_", 0) == 0 && EndsWith(name, ".ini"));
}

std::string ImportName(const std::filesystem::path& source)
{
    if (!IsLayoutPath(source))
    {
        return source.stem().string();
    }
    const std::string track = source.parent_path().filename().string();
    const std::string stem = source.stem().string();
    // "models_endurance.ini" -> "<track>_endurance"; plain "models.ini" -> "<track>".
    return stem.size() > 7 ? track + "_" + stem.substr(7) : track;
}

std::vector<Kn5LayoutModel> ReadLayout(const std::filesystem::path& layoutPath)
{
    std::vector<Kn5LayoutModel> models;
    std::vector<std::string> missing;
    for (const auto& [section, values] : ReadAcIni(layoutPath))
    {
        const auto file = values.find("FILE");
        if (ToLowerAscii(section).rfind("model", 0) != 0 || file == values.end() || file->second.empty())
        {
            continue;
        }
        Kn5LayoutModel model;
        model.file = layoutPath.parent_path() / std::filesystem::path(file->second).make_preferred();
        model.position = ParseTriple(values, "POSITION");
        model.rotationDegrees = ParseTriple(values, "ROTATION");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(model.file, ec))
        {
            missing.push_back(file->second);
        }
        models.push_back(std::move(model));
    }
    if (models.empty())
    {
        throw std::runtime_error("'" + layoutPath.string() + "' places no models (no [MODEL_n] section with a FILE)");
    }
    if (!missing.empty())
    {
        std::string list;
        for (const std::string& name : missing)
        {
            list += (list.empty() ? "" : ", ") + name;
        }
        throw std::runtime_error("'" + layoutPath.filename().string() + "' names models that are not there: " + list);
    }
    return models;
}

std::vector<std::filesystem::path> FindLayouts(const std::filesystem::path& kn5Path)
{
    std::vector<std::filesystem::path> layouts;
    std::error_code ec;
    const std::filesystem::path wanted = std::filesystem::absolute(kn5Path, ec).lexically_normal();
    for (std::filesystem::directory_iterator it(kn5Path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !IsLayoutPath(it->path()))
        {
            continue;
        }
        try
        {
            for (const Kn5LayoutModel& model : ReadLayout(it->path()))
            {
                std::error_code absoluteEc;
                const std::filesystem::path placed = std::filesystem::absolute(model.file, absoluteEc).lexically_normal();
                // The game resolves FILE ignoring case.
                if (ToLowerAscii(placed.generic_string()) == ToLowerAscii(wanted.generic_string()))
                {
                    layouts.push_back(it->path());
                    break;
                }
            }
        }
        catch (const std::exception&)
        {
            // A broken layout is not offered; importing it directly reports why.
        }
    }
    std::sort(layouts.begin(), layouts.end());
    return layouts;
}

std::array<float, 16> LayoutModelMatrix(const std::array<float, 3>& position, const std::array<float, 3>& rotationDegrees)
{
    constexpr float kRadiansPerDegree = 3.14159265358979323846f / 180.0f;
    const float cx = std::cos(rotationDegrees[0] * kRadiansPerDegree);
    const float sx = std::sin(rotationDegrees[0] * kRadiansPerDegree);
    const float cy = std::cos(rotationDegrees[1] * kRadiansPerDegree);
    const float sy = std::sin(rotationDegrees[1] * kRadiansPerDegree);
    const float cz = std::cos(rotationDegrees[2] * kRadiansPerDegree);
    const float sz = std::sin(rotationDegrees[2] * kRadiansPerDegree);
    // Rows of Rz * Ry * Rx.
    const float m[3][3] = {
        {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
        {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
        {-sy, cy * sx, cy * cx}};
    return {m[0][0], m[1][0], m[2][0], 0.0f,
            m[0][1], m[1][1], m[2][1], 0.0f,
            m[0][2], m[1][2], m[2][2], 0.0f,
            position[0], position[1], position[2], 1.0f};
}

Kn5ModelSummary Inspect(const std::filesystem::path& source)
{
    Kn5ModelSummary summary;
    if (IsLayoutPath(source))
    {
        for (const Kn5LayoutModel& layoutModel : ReadLayout(source))
        {
            summary.encrypted = summary.encrypted || Kn5Reader::IsEncrypted(layoutModel.file);
            Kn5Model model = Kn5Reader::Load(layoutModel.file, Kn5ReadScope::NoGeometry);
            FoldTextureCase(model);
            SurveyModel(model, summary);
        }
        // Tracks have no liveries: only their own textures.
        summary.skins.push_back(Kn5SkinSummary{});
        return summary;
    }

    summary.encrypted = Kn5Reader::IsEncrypted(source);
    Kn5Model model = Kn5Reader::Load(source, Kn5ReadScope::NoGeometry);
    FoldTextureCase(model);
    const std::vector<size_t> materialTriangles = SurveyModel(model, summary);
    for (const std::filesystem::path& layout : FindLayouts(source))
    {
        summary.layouts.push_back({layout, ReadLayout(layout).size()});
    }
    if (summary.encrypted)
    {
        // Its textures are decoys: no colour read from them would be the real one.
        return summary;
    }
    SurveySkins(source, model, materialTriangles, summary);
    return summary;
}

Kn5ImportReport ConvertToGltf(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory,
    const Kn5ImportOptions& options)
{
    const std::vector<Kn5LayoutModel> sources = ImportSources(source);
    const bool layout = IsLayoutPath(source);
    // Refused before anything is written.
    for (const Kn5LayoutModel& placed : sources)
    {
        if (Kn5Reader::IsEncrypted(placed.file))
        {
            throw std::runtime_error(
                "Refusing '" + placed.file.filename().string() +
                "': it carries the CSP kn5 encryption trailer. Its textures and several meshes are decoys in the plain "
                "section, so the import would be wrong without looking it.");
        }
    }

    const std::string name = ImportName(source);
    const std::filesystem::path gltfPath = targetDirectory / (name + ".gltf");
    const std::filesystem::path binaryPath = targetDirectory / "buffers" / (name + ".bin");
    std::error_code existsEc;
    if (std::filesystem::exists(gltfPath, existsEc) || std::filesystem::exists(binaryPath, existsEc))
    {
        throw std::runtime_error("'" + gltfPath.string() + "' already exists; an import does not overwrite it");
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

    GltfBuilder builder(textureDirectory, options);
    Kn5ImportReport& report = builder.Report();
    // Tracks have no liveries; a car's skin replaces textures in every pass that reads them.
    const std::optional<std::filesystem::path> skin =
        layout ? std::nullopt : ResolveSkinDirectory(sources.front().file, options.skin);

    // Three passes, one model at a time, each reading only what it needs: a track's kn5 run to
    // hundreds of megabytes. First which textures the whole import samples, then those textures
    // from whichever model carries each, then materials and geometry.
    for (const Kn5LayoutModel& placed : sources)
    {
        Kn5Model tables = Kn5Reader::Load(placed.file, Kn5ReadScope::Tables);
        FoldTextureCase(tables);
        builder.CollectTextureUse(tables);
    }
    for (const Kn5LayoutModel& placed : sources)
    {
        Kn5Model textures = Kn5Reader::Load(placed.file, Kn5ReadScope::Textures);
        FoldTextureCase(textures);
        if (skin.has_value())
        {
            ApplySkin(textures, *skin);
        }
        builder.WriteTextures(textures);
    }

    std::vector<size_t> roots;
    for (const Kn5LayoutModel& placed : sources)
    {
        Kn5Model model = Kn5Reader::Load(placed.file);
        report.foldedTextureNames += FoldTextureCase(model);
        ++report.models;
        if (skin.has_value())
        {
            report.skin = skin->filename().string();
            report.skinTextures = ApplySkin(model, *skin).size();
        }

        builder.AddMaterials(model);
        std::vector<std::string> names;
        if (!options.keepVariants)
        {
            CollectNodeNames(model.root, names);
        }
        builder.SetLowResTwins(LowResTwins(names));

        std::optional<size_t> root = builder.Emit(model.root);
        if (!root.has_value())
        {
            continue;
        }
        if (placed.position != std::array<float, 3>{} || placed.rotationDegrees != std::array<float, 3>{})
        {
            // Under the axis root, so the placement is written in AC's frame like everything else.
            root = builder.AddPlacement(placed.file.filename().string(), LayoutModelMatrix(placed.position, placed.rotationDegrees), *root);
        }
        roots.push_back(*root);
    }
    const size_t sceneRoot = builder.AddRoot(name, roots);

    const std::string binaryUri = "buffers/" + name + ".bin";
    const Json document = builder.BuildDocument(sceneRoot, binaryUri);
    WriteFileBytes(binaryPath, builder.Binary().data(), builder.Binary().size());
    const std::string text = document.dump();
    WriteFileBytes(gltfPath, text.data(), text.size());

    report.gltfPath = gltfPath;
    report.nodes = builder.NodeCount();
    report.images = builder.ImageCount();

    const std::string sourceName = source.filename().string();
    LOG_INFO(
        "kn5 '{}': {} model(s), {} nodes ({} transforms, {} meshes, {} empty, {} variants dropped, {} never rendered), {} triangles, "
        "{} images, {} materials",
        sourceName,
        report.models,
        report.nodes,
        report.transforms,
        report.meshes,
        report.emptyMeshes,
        report.droppedVariants,
        report.hiddenMeshes,
        report.triangles,
        report.images,
        report.materials);
    if (!report.skin.empty())
    {
        LOG_INFO("kn5 '{}': skin '{}' ({} textures)", sourceName, report.skin, report.skinTextures);
    }
    if (report.foldedTextureNames > 0)
    {
        LOG_INFO("kn5 '{}': folded {} texture names that differed only in case", sourceName, report.foldedTextureNames);
    }
    if (report.scrubbedAttributes > 0 || report.scrubbedMatrices > 0)
    {
        LOG_WARN(
            "kn5 '{}': replaced {} non-finite vertex attributes and dropped {} non-finite node matrices",
            sourceName,
            report.scrubbedAttributes,
            report.scrubbedMatrices);
    }
    return report;
}
}
}
