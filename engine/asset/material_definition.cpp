#include "material_definition.h"

#include "material_graph_runtime.h"

#include <algorithm>
#include <exception>
#include <cctype>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace me
{

namespace
{
// A sampler's fields as a sidecar writes them.
const char* TextureWrapName(TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return "clamp_to_edge";
    case TextureWrap::MirroredRepeat:
        return "mirrored_repeat";
    default:
        return "repeat";
    }
}

const char* TextureFilterName(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? "nearest" : "linear";
}

const char* TextureMipFilterName(TextureMipFilter filter)
{
    switch (filter)
    {
    case TextureMipFilter::Nearest:
        return "nearest";
    case TextureMipFilter::None:
        return "none";
    default:
        return "linear";
    }
}

// The reverse; an empty or unknown value keeps the fallback.
TextureWrap ParseTextureWrap(const std::string& value, TextureWrap fallback)
{
    for (const TextureWrap wrap : {TextureWrap::Repeat, TextureWrap::ClampToEdge, TextureWrap::MirroredRepeat})
    {
        if (value == TextureWrapName(wrap))
        {
            return wrap;
        }
    }
    return fallback;
}

TextureFilter ParseTextureFilter(const std::string& value, TextureFilter fallback)
{
    for (const TextureFilter filter : {TextureFilter::Linear, TextureFilter::Nearest})
    {
        if (value == TextureFilterName(filter))
        {
            return filter;
        }
    }
    return fallback;
}

TextureMipFilter ParseTextureMipFilter(const std::string& value, TextureMipFilter fallback)
{
    for (const TextureMipFilter filter : {TextureMipFilter::Linear, TextureMipFilter::Nearest, TextureMipFilter::None})
    {
        if (value == TextureMipFilterName(filter))
        {
            return filter;
        }
    }
    return fallback;
}

template <size_t Count>
void ReadFloatSequence(const YAML::Node& node, float (&destination)[Count])
{
    if (!node || !node.IsSequence())
    {
        return;
    }

    const size_t count = std::min(Count, node.size());
    for (size_t index = 0; index < count; ++index)
    {
        destination[index] = node[index].as<float>(destination[index]);
    }
}

void SerializeFloatSequence(YAML::Node& node, const float* values, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        node.push_back(values[index]);
    }
}

bool HasBlendData(const MaterialTextureBlendGraph& blendGraph)
{
    return blendGraph.enabled ||
           !blendGraph.blendMaskTexturePath.empty() ||
           !blendGraph.secondaryBaseColorTexturePath.empty() ||
           !blendGraph.secondaryNormalTexturePath.empty() ||
           !blendGraph.secondaryMetallicTexturePath.empty() ||
           !blendGraph.secondaryRoughnessTexturePath.empty() ||
           !blendGraph.secondaryOcclusionTexturePath.empty() ||
           !blendGraph.secondaryEmissiveTexturePath.empty();
}
}

std::filesystem::path BuildMaterialDefinitionPath(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex)
{
    return modelPath.parent_path() /
           (modelPath.stem().string() + "_" + std::to_string(materialIndex) + ".material.yaml");
}

std::vector<std::filesystem::path> FindMaterialDefinitionFiles(const std::filesystem::path& modelPath)
{
    const std::string prefix = modelPath.stem().string() + "_";
    constexpr std::string_view kSuffix = ".material.yaml";

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(modelPath.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(kSuffix))
        {
            continue;
        }
        // Only a numeric index: "tree_1.glb" is another model whose own
        // definitions ("tree_1_0.material.yaml") must not be claimed by "tree".
        const std::string index = name.substr(prefix.size(), name.size() - prefix.size() - kSuffix.size());
        const bool numeric = !index.empty() && std::all_of(index.begin(), index.end(), [](unsigned char c)
                                                           {
                                                               return std::isdigit(c) != 0;
                                                           });
        if (numeric)
        {
            files.push_back(it->path());
        }
    }
    return files;
}

std::optional<uint32_t> MaterialDefinitionIndex(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& definitionPath)
{
    const std::string prefix = modelPath.stem().string() + "_";
    constexpr std::string_view kSuffix = ".material.yaml";
    const std::string name = definitionPath.filename().string();
    if (!name.starts_with(prefix) || !name.ends_with(kSuffix) || name.size() <= prefix.size() + kSuffix.size())
    {
        return std::nullopt;
    }
    try
    {
        return static_cast<uint32_t>(
            std::stoul(name.substr(prefix.size(), name.size() - prefix.size() - kSuffix.size())));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<std::string> ReadMaterialDefinitionName(const std::filesystem::path& definitionPath)
{
    try
    {
        const YAML::Node name = YAML::LoadFile(definitionPath.string())["material"]["name"];
        if (name && name.IsScalar())
        {
            return name.as<std::string>();
        }
    }
    catch (const YAML::Exception&)
    {
    }
    return std::nullopt;
}

ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material)
{
    return ModelImportedMaterialInfo{
        material.name,
        material.baseColorTexturePath,
        material.normalTexturePath,
        material.metallicTexturePath,
        material.roughnessTexturePath,
        material.occlusionTexturePath,
        material.emissiveTexturePath,
        material.clearcoatTexturePath,
        material.clearcoatRoughnessTexturePath,
        material.sheenColorTexturePath,
        material.sheenRoughnessTexturePath,
        material.anisotropyTexturePath,
        material.specularTexturePath,
        material.specularColorTexturePath,
        material.clearcoatNormalTexturePath,
        material.iridescenceTexturePath,
        material.iridescenceThicknessTexturePath,
        material.transmissionTexturePath,
        material.thicknessTexturePath,
        material.pbr,
        material.blendGraph,
        material.shaderGraph,
        material.textureTransforms,
        material.textureSamplers};
}

void ApplyImportedMaterialInfo(const ModelImportedMaterialInfo& source, ModelMaterialData& destination)
{
    destination.name = source.name;
    destination.baseColorTexturePath = source.baseColorTexturePath;
    destination.normalTexturePath = source.normalTexturePath;
    destination.metallicTexturePath = source.metallicTexturePath;
    destination.roughnessTexturePath = source.roughnessTexturePath;
    destination.occlusionTexturePath = source.occlusionTexturePath;
    destination.emissiveTexturePath = source.emissiveTexturePath;
    destination.clearcoatTexturePath = source.clearcoatTexturePath;
    destination.clearcoatRoughnessTexturePath = source.clearcoatRoughnessTexturePath;
    destination.sheenColorTexturePath = source.sheenColorTexturePath;
    destination.sheenRoughnessTexturePath = source.sheenRoughnessTexturePath;
    destination.anisotropyTexturePath = source.anisotropyTexturePath;
    destination.specularTexturePath = source.specularTexturePath;
    destination.specularColorTexturePath = source.specularColorTexturePath;
    destination.clearcoatNormalTexturePath = source.clearcoatNormalTexturePath;
    destination.iridescenceTexturePath = source.iridescenceTexturePath;
    destination.iridescenceThicknessTexturePath = source.iridescenceThicknessTexturePath;
    destination.transmissionTexturePath = source.transmissionTexturePath;
    destination.thicknessTexturePath = source.thicknessTexturePath;
    destination.textureTransforms = source.textureTransforms;
    destination.textureSamplers = source.textureSamplers;
    destination.unlit = source.pbr.unlit;
    destination.pbr = source.pbr;
    destination.blendGraph = source.blendGraph;
    destination.shaderGraph = source.shaderGraph;
    for (size_t index = 0; index < 4; ++index)
    {
        destination.baseColor[index] = source.pbr.baseColorFactor[index];
    }
    for (size_t index = 0; index < 3; ++index)
    {
        destination.emissiveColor[index] = source.pbr.emissiveColor[index];
    }
    destination.metallicFactor = source.pbr.metallicFactor;
    destination.roughnessFactor = source.pbr.roughnessFactor;
    destination.normalScale = source.pbr.normalScale;
    destination.occlusionStrength = source.pbr.occlusionStrength;
    destination.emissiveIntensity = source.pbr.emissiveIntensity;
    destination.clearcoatFactor = source.pbr.clearcoatFactor;
    destination.clearcoatRoughnessFactor = source.pbr.clearcoatRoughnessFactor;
    for (size_t index = 0; index < 3; ++index)
    {
        destination.sheenColorFactor[index] = source.pbr.sheenColorFactor[index];
    }
    destination.sheenRoughnessFactor = source.pbr.sheenRoughnessFactor;
    destination.anisotropyStrength = source.pbr.anisotropyStrength;
    destination.anisotropyRotation = source.pbr.anisotropyRotation;
    destination.ior = source.pbr.ior;
    destination.specularFactor = source.pbr.specularFactor;
    for (size_t index = 0; index < 3; ++index)
    {
        destination.specularColorFactor[index] = source.pbr.specularColorFactor[index];
    }
    destination.clearcoatNormalScale = source.pbr.clearcoatNormalScale;
    destination.iridescenceFactor = source.pbr.iridescenceFactor;
    destination.iridescenceIor = source.pbr.iridescenceIor;
    destination.iridescenceThicknessMinimum = source.pbr.iridescenceThicknessMinimum;
    destination.iridescenceThicknessMaximum = source.pbr.iridescenceThicknessMaximum;
    destination.transmissionFactor = source.pbr.transmissionFactor;
    destination.thicknessFactor = source.pbr.thicknessFactor;
    destination.attenuationDistance = source.pbr.attenuationDistance;
    for (size_t index = 0; index < 3; ++index)
    {
        destination.attenuationColor[index] = source.pbr.attenuationColor[index];
    }
    destination.opacity = ClampMaterialAlphaValue(source.pbr.opacity, 1.0f);
    destination.alphaMode = source.pbr.alphaMode;
    destination.alphaCutoff = ClampMaterialAlphaValue(source.pbr.alphaCutoff, 0.5f);
    destination.pbr.opacity = destination.opacity;
    destination.pbr.alphaCutoff = destination.alphaCutoff;
}

YAML::Node SerializeMaterialDefinition(const ModelImportedMaterialInfo& material)
{
    YAML::Node node(YAML::NodeType::Map);
    node["name"] = material.name;
    node["base_color_texture_path"] = material.baseColorTexturePath;
    node["normal_texture_path"] = material.normalTexturePath;
    node["metallic_texture_path"] = material.metallicTexturePath;
    node["roughness_texture_path"] = material.roughnessTexturePath;
    node["occlusion_texture_path"] = material.occlusionTexturePath;
    node["emissive_texture_path"] = material.emissiveTexturePath;
    node["clearcoat_texture_path"] = material.clearcoatTexturePath;
    node["clearcoat_roughness_texture_path"] = material.clearcoatRoughnessTexturePath;
    node["sheen_color_texture_path"] = material.sheenColorTexturePath;
    node["sheen_roughness_texture_path"] = material.sheenRoughnessTexturePath;
    node["anisotropy_texture_path"] = material.anisotropyTexturePath;
    node["specular_texture_path"] = material.specularTexturePath;
    node["specular_color_texture_path"] = material.specularColorTexturePath;
    node["clearcoat_normal_texture_path"] = material.clearcoatNormalTexturePath;
    node["iridescence_texture_path"] = material.iridescenceTexturePath;
    node["iridescence_thickness_texture_path"] = material.iridescenceThicknessTexturePath;
    node["transmission_texture_path"] = material.transmissionTexturePath;
    node["thickness_texture_path"] = material.thicknessTexturePath;

    YAML::Node pbr(YAML::NodeType::Map);
    YAML::Node baseColor(YAML::NodeType::Sequence);
    SerializeFloatSequence(baseColor, material.pbr.baseColorFactor, 4);
    YAML::Node emissiveColor(YAML::NodeType::Sequence);
    SerializeFloatSequence(emissiveColor, material.pbr.emissiveColor, 3);
    pbr["base_color_factor"] = baseColor;
    pbr["emissive_color"] = emissiveColor;
    pbr["metallic_factor"] = material.pbr.metallicFactor;
    pbr["roughness_factor"] = material.pbr.roughnessFactor;
    pbr["normal_scale"] = material.pbr.normalScale;
    pbr["occlusion_strength"] = material.pbr.occlusionStrength;
    pbr["emissive_intensity"] = material.pbr.emissiveIntensity;
    pbr["alpha_mode"] = ToString(material.pbr.alphaMode);
    pbr["alpha_cutoff"] = ClampMaterialAlphaValue(material.pbr.alphaCutoff, 0.5f);
    pbr["opacity"] = ClampMaterialAlphaValue(material.pbr.opacity, 1.0f);
    pbr["clearcoat_factor"] = material.pbr.clearcoatFactor;
    pbr["clearcoat_roughness_factor"] = material.pbr.clearcoatRoughnessFactor;
    YAML::Node sheenColor(YAML::NodeType::Sequence);
    SerializeFloatSequence(sheenColor, material.pbr.sheenColorFactor, 3);
    pbr["sheen_color_factor"] = sheenColor;
    pbr["sheen_roughness_factor"] = material.pbr.sheenRoughnessFactor;
    pbr["anisotropy_strength"] = material.pbr.anisotropyStrength;
    pbr["anisotropy_rotation"] = material.pbr.anisotropyRotation;
    pbr["ior"] = material.pbr.ior;
    pbr["specular_factor"] = material.pbr.specularFactor;
    YAML::Node specularColor(YAML::NodeType::Sequence);
    SerializeFloatSequence(specularColor, material.pbr.specularColorFactor, 3);
    pbr["specular_color_factor"] = specularColor;
    pbr["clearcoat_normal_scale"] = material.pbr.clearcoatNormalScale;
    pbr["iridescence_factor"] = material.pbr.iridescenceFactor;
    pbr["iridescence_ior"] = material.pbr.iridescenceIor;
    pbr["iridescence_thickness_minimum"] = material.pbr.iridescenceThicknessMinimum;
    pbr["iridescence_thickness_maximum"] = material.pbr.iridescenceThicknessMaximum;
    pbr["transmission_factor"] = material.pbr.transmissionFactor;
    pbr["thickness_factor"] = material.pbr.thicknessFactor;
    pbr["attenuation_distance"] = material.pbr.attenuationDistance;
    YAML::Node attenuationColor(YAML::NodeType::Sequence);
    SerializeFloatSequence(attenuationColor, material.pbr.attenuationColor, 3);
    pbr["attenuation_color"] = attenuationColor;
    pbr["unlit"] = material.pbr.unlit;
    node["pbr"] = pbr;

    // Only the transforms that do something, keyed by slot name.
    YAML::Node transforms(YAML::NodeType::Map);
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        const TextureTransform& transform = material.textureTransforms[slot];
        const char* name = MaterialTextureSlotName(slot);
        if (name == nullptr || transform.IsIdentity())
        {
            continue;
        }
        YAML::Node entry(YAML::NodeType::Map);
        YAML::Node offset(YAML::NodeType::Sequence);
        SerializeFloatSequence(offset, transform.offset, 2);
        YAML::Node scale(YAML::NodeType::Sequence);
        SerializeFloatSequence(scale, transform.scale, 2);
        entry["offset"] = offset;
        entry["rotation"] = transform.rotation;
        entry["scale"] = scale;
        entry["tex_coord"] = transform.texCoord;
        transforms[name] = entry;
    }
    if (transforms.size() > 0)
    {
        node["texture_transforms"] = transforms;
    }

    // Only the samplers that differ from the default, keyed by slot name.
    YAML::Node samplers(YAML::NodeType::Map);
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        const TextureSampler& sampler = material.textureSamplers[slot];
        const char* name = MaterialTextureSlotName(slot);
        if (name == nullptr || sampler.IsDefault())
        {
            continue;
        }
        YAML::Node entry(YAML::NodeType::Map);
        entry["wrap_s"] = TextureWrapName(sampler.wrapS);
        entry["wrap_t"] = TextureWrapName(sampler.wrapT);
        entry["mag_filter"] = TextureFilterName(sampler.magFilter);
        entry["min_filter"] = TextureFilterName(sampler.minFilter);
        entry["mip_filter"] = TextureMipFilterName(sampler.mipFilter);
        samplers[name] = entry;
    }
    if (samplers.size() > 0)
    {
        node["texture_samplers"] = samplers;
    }

    if (HasBlendData(material.blendGraph))
    {
        const MaterialTextureBlendGraph& blendGraph = material.blendGraph;
        YAML::Node graph(YAML::NodeType::Map);
        graph["enabled"] = blendGraph.enabled;
        graph["blend_factor"] = blendGraph.blendFactor;
        graph["blend_mask_texture_path"] = blendGraph.blendMaskTexturePath;
        graph["secondary_base_color_texture_path"] = blendGraph.secondaryBaseColorTexturePath;
        graph["secondary_normal_texture_path"] = blendGraph.secondaryNormalTexturePath;
        graph["secondary_metallic_texture_path"] = blendGraph.secondaryMetallicTexturePath;
        graph["secondary_roughness_texture_path"] = blendGraph.secondaryRoughnessTexturePath;
        graph["secondary_occlusion_texture_path"] = blendGraph.secondaryOcclusionTexturePath;
        graph["secondary_emissive_texture_path"] = blendGraph.secondaryEmissiveTexturePath;
        node["texture_graph"] = graph;
    }

    if (!material.shaderGraph.IsEmpty())
    {
        node["shader_graph"] = SerializeMaterialShaderGraph(material.shaderGraph);
    }
    return node;
}

bool LoadMaterialDefinition(
    const std::filesystem::path& path,
    ModelImportedMaterialInfo& material,
    std::string& warning)
{
    warning.clear();
    try
    {
        const YAML::Node root = YAML::LoadFile(path.string());
        const YAML::Node node = root["material"];
        if (!node || !node.IsMap())
        {
            warning = "Material definition is missing its material map";
            return false;
        }

        material.name = node["name"].as<std::string>(material.name);
        material.baseColorTexturePath = node["base_color_texture_path"].as<std::string>(material.baseColorTexturePath);
        material.normalTexturePath = node["normal_texture_path"].as<std::string>(material.normalTexturePath);
        material.metallicTexturePath = node["metallic_texture_path"].as<std::string>(material.metallicTexturePath);
        material.roughnessTexturePath = node["roughness_texture_path"].as<std::string>(material.roughnessTexturePath);
        material.occlusionTexturePath = node["occlusion_texture_path"].as<std::string>(material.occlusionTexturePath);
        material.emissiveTexturePath = node["emissive_texture_path"].as<std::string>(material.emissiveTexturePath);
        // Absent in sidecars written before the layer maps existed, which then keep what they had.
        material.clearcoatTexturePath = node["clearcoat_texture_path"].as<std::string>(material.clearcoatTexturePath);
        material.clearcoatRoughnessTexturePath =
            node["clearcoat_roughness_texture_path"].as<std::string>(material.clearcoatRoughnessTexturePath);
        material.sheenColorTexturePath = node["sheen_color_texture_path"].as<std::string>(material.sheenColorTexturePath);
        material.sheenRoughnessTexturePath = node["sheen_roughness_texture_path"].as<std::string>(material.sheenRoughnessTexturePath);
        material.anisotropyTexturePath = node["anisotropy_texture_path"].as<std::string>(material.anisotropyTexturePath);
        material.specularTexturePath = node["specular_texture_path"].as<std::string>(material.specularTexturePath);
        material.specularColorTexturePath = node["specular_color_texture_path"].as<std::string>(material.specularColorTexturePath);
        material.clearcoatNormalTexturePath = node["clearcoat_normal_texture_path"].as<std::string>(material.clearcoatNormalTexturePath);
        material.iridescenceTexturePath = node["iridescence_texture_path"].as<std::string>(material.iridescenceTexturePath);
        material.iridescenceThicknessTexturePath =
            node["iridescence_thickness_texture_path"].as<std::string>(material.iridescenceThicknessTexturePath);
        material.transmissionTexturePath = node["transmission_texture_path"].as<std::string>(material.transmissionTexturePath);
        material.thicknessTexturePath = node["thickness_texture_path"].as<std::string>(material.thicknessTexturePath);

        // Absent in sidecars written before transforms existed: every texture keeps its own.
        if (const YAML::Node transformsNode = node["texture_transforms"]; transformsNode && transformsNode.IsMap())
        {
            for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
            {
                const char* name = MaterialTextureSlotName(slot);
                if (name == nullptr || !transformsNode[name] || !transformsNode[name].IsMap())
                {
                    continue;
                }
                const YAML::Node entry = transformsNode[name];
                TextureTransform& transform = material.textureTransforms[slot];
                ReadFloatSequence(entry["offset"], transform.offset);
                transform.rotation = entry["rotation"].as<float>(transform.rotation);
                ReadFloatSequence(entry["scale"], transform.scale);
                transform.texCoord = std::min(entry["tex_coord"].as<uint32_t>(transform.texCoord), 1u);
            }
        }

        // Absent in sidecars written before samplers were read: every texture keeps the default.
        if (const YAML::Node samplersNode = node["texture_samplers"]; samplersNode && samplersNode.IsMap())
        {
            for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
            {
                const char* name = MaterialTextureSlotName(slot);
                if (name == nullptr || !samplersNode[name] || !samplersNode[name].IsMap())
                {
                    continue;
                }
                const YAML::Node entry = samplersNode[name];
                TextureSampler& sampler = material.textureSamplers[slot];
                sampler.wrapS = ParseTextureWrap(entry["wrap_s"].as<std::string>(""), sampler.wrapS);
                sampler.wrapT = ParseTextureWrap(entry["wrap_t"].as<std::string>(""), sampler.wrapT);
                sampler.magFilter = ParseTextureFilter(entry["mag_filter"].as<std::string>(""), sampler.magFilter);
                sampler.minFilter = ParseTextureFilter(entry["min_filter"].as<std::string>(""), sampler.minFilter);
                sampler.mipFilter = ParseTextureMipFilter(entry["mip_filter"].as<std::string>(""), sampler.mipFilter);
            }
        }

        if (const YAML::Node pbrNode = node["pbr"]; pbrNode && pbrNode.IsMap())
        {
            ReadFloatSequence(pbrNode["base_color_factor"], material.pbr.baseColorFactor);
            ReadFloatSequence(pbrNode["emissive_color"], material.pbr.emissiveColor);
            material.pbr.metallicFactor = pbrNode["metallic_factor"].as<float>(material.pbr.metallicFactor);
            material.pbr.roughnessFactor = pbrNode["roughness_factor"].as<float>(material.pbr.roughnessFactor);
            material.pbr.normalScale = pbrNode["normal_scale"].as<float>(material.pbr.normalScale);
            material.pbr.occlusionStrength = pbrNode["occlusion_strength"].as<float>(material.pbr.occlusionStrength);
            material.pbr.emissiveIntensity = pbrNode["emissive_intensity"].as<float>(material.pbr.emissiveIntensity);
            // Absent in sidecars written before clearcoat existed, which then keep what they had.
            material.pbr.clearcoatFactor = std::clamp(
                pbrNode["clearcoat_factor"].as<float>(material.pbr.clearcoatFactor),
                0.0f,
                1.0f);
            material.pbr.clearcoatRoughnessFactor = std::clamp(
                pbrNode["clearcoat_roughness_factor"].as<float>(material.pbr.clearcoatRoughnessFactor),
                0.0f,
                1.0f);
            ReadFloatSequence(pbrNode["sheen_color_factor"], material.pbr.sheenColorFactor);
            for (float& component : material.pbr.sheenColorFactor)
            {
                component = std::clamp(component, 0.0f, 1.0f);
            }
            material.pbr.sheenRoughnessFactor = std::clamp(
                pbrNode["sheen_roughness_factor"].as<float>(material.pbr.sheenRoughnessFactor),
                0.0f,
                1.0f);
            material.pbr.anisotropyStrength = std::clamp(
                pbrNode["anisotropy_strength"].as<float>(material.pbr.anisotropyStrength),
                0.0f,
                1.0f);
            material.pbr.anisotropyRotation = pbrNode["anisotropy_rotation"].as<float>(material.pbr.anisotropyRotation);
            material.pbr.ior = SanitizeIor(pbrNode["ior"].as<float>(material.pbr.ior));
            material.pbr.specularFactor = std::clamp(pbrNode["specular_factor"].as<float>(material.pbr.specularFactor), 0.0f, 1.0f);
            ReadFloatSequence(pbrNode["specular_color_factor"], material.pbr.specularColorFactor);
            for (float& component : material.pbr.specularColorFactor)
            {
                component = std::max(component, 0.0f);
            }
            material.pbr.clearcoatNormalScale = pbrNode["clearcoat_normal_scale"].as<float>(material.pbr.clearcoatNormalScale);
            material.pbr.iridescenceFactor = std::clamp(pbrNode["iridescence_factor"].as<float>(material.pbr.iridescenceFactor), 0.0f, 1.0f);
            material.pbr.iridescenceIor = std::max(pbrNode["iridescence_ior"].as<float>(material.pbr.iridescenceIor), 1.0f);
            material.pbr.iridescenceThicknessMinimum =
                std::max(pbrNode["iridescence_thickness_minimum"].as<float>(material.pbr.iridescenceThicknessMinimum), 0.0f);
            material.pbr.iridescenceThicknessMaximum =
                std::max(pbrNode["iridescence_thickness_maximum"].as<float>(material.pbr.iridescenceThicknessMaximum), 0.0f);
            // Absent in sidecars written before transmission existed: opaque, thin, no absorption.
            material.pbr.transmissionFactor =
                std::clamp(pbrNode["transmission_factor"].as<float>(material.pbr.transmissionFactor), 0.0f, 1.0f);
            material.pbr.thicknessFactor = std::max(pbrNode["thickness_factor"].as<float>(material.pbr.thicknessFactor), 0.0f);
            material.pbr.attenuationDistance =
                std::max(pbrNode["attenuation_distance"].as<float>(material.pbr.attenuationDistance), 0.0f);
            ReadFloatSequence(pbrNode["attenuation_color"], material.pbr.attenuationColor);
            for (float& component : material.pbr.attenuationColor)
            {
                component = std::clamp(component, 0.0f, 1.0f);
            }
            material.pbr.unlit = pbrNode["unlit"].as<bool>(material.pbr.unlit);
            const std::string storedMode = pbrNode["alpha_mode"].as<std::string>(ToString(material.pbr.alphaMode));
            if (const std::optional<MaterialAlphaMode> parsed = ParseMaterialAlphaMode(storedMode))
            {
                material.pbr.alphaMode = *parsed;
            }
            else
            {
                material.pbr.alphaMode = MaterialAlphaMode::Opaque;
                warning = "Unknown material alpha_mode '" + storedMode + "'; using opaque";
            }
            const float cutoffFallback = ClampMaterialAlphaValue(material.pbr.alphaCutoff, 0.5f);
            material.pbr.alphaCutoff = ClampMaterialAlphaValue(
                pbrNode["alpha_cutoff"].as<float>(cutoffFallback),
                cutoffFallback);
            const float opacityFallback = ClampMaterialAlphaValue(material.pbr.opacity, 1.0f);
            material.pbr.opacity = ClampMaterialAlphaValue(
                pbrNode["opacity"].as<float>(opacityFallback),
                opacityFallback);
        }

        if (const YAML::Node graph = node["texture_graph"]; graph && graph.IsMap())
        {
            MaterialTextureBlendGraph& blendGraph = material.blendGraph;
            blendGraph.enabled = graph["enabled"].as<bool>(blendGraph.enabled);
            blendGraph.blendFactor = graph["blend_factor"].as<float>(blendGraph.blendFactor);
            blendGraph.blendMaskTexturePath = graph["blend_mask_texture_path"].as<std::string>(blendGraph.blendMaskTexturePath);
            blendGraph.secondaryBaseColorTexturePath = graph["secondary_base_color_texture_path"].as<std::string>(blendGraph.secondaryBaseColorTexturePath);
            blendGraph.secondaryNormalTexturePath = graph["secondary_normal_texture_path"].as<std::string>(blendGraph.secondaryNormalTexturePath);
            blendGraph.secondaryMetallicTexturePath = graph["secondary_metallic_texture_path"].as<std::string>(blendGraph.secondaryMetallicTexturePath);
            blendGraph.secondaryRoughnessTexturePath = graph["secondary_roughness_texture_path"].as<std::string>(blendGraph.secondaryRoughnessTexturePath);
            blendGraph.secondaryOcclusionTexturePath = graph["secondary_occlusion_texture_path"].as<std::string>(blendGraph.secondaryOcclusionTexturePath);
            blendGraph.secondaryEmissiveTexturePath = graph["secondary_emissive_texture_path"].as<std::string>(blendGraph.secondaryEmissiveTexturePath);
        }

        if (const YAML::Node shaderGraph = node["shader_graph"])
        {
            DeserializeMaterialShaderGraph(shaderGraph, material.name, std::nullopt, material);
            for (MaterialShaderNode& shaderNode : material.shaderGraph.nodes)
            {
                if (shaderNode.type == MaterialShaderNodeType::Output)
                {
                    shaderNode.pbr.alphaMode = material.pbr.alphaMode;
                    shaderNode.pbr.alphaCutoff = material.pbr.alphaCutoff;
                    break;
                }
            }
        }
        return true;
    }
    catch (const YAML::Exception& exception)
    {
        warning = exception.what();
        return false;
    }
}
}
