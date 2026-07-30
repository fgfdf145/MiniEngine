#include "material_definition.h"

#include "material_graph_runtime.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace
{
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
    uint32_t materialIndex
)
{
    return modelPath.parent_path() /
        (modelPath.stem().string() + "_" + std::to_string(materialIndex) + ".material.yaml");
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
        material.pbr,
        material.blendGraph,
        material.shaderGraph
    };
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
    node["pbr"] = pbr;

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
    std::string& warning
)
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

        if (const YAML::Node pbrNode = node["pbr"]; pbrNode && pbrNode.IsMap())
        {
            ReadFloatSequence(pbrNode["base_color_factor"], material.pbr.baseColorFactor);
            ReadFloatSequence(pbrNode["emissive_color"], material.pbr.emissiveColor);
            material.pbr.metallicFactor = pbrNode["metallic_factor"].as<float>(material.pbr.metallicFactor);
            material.pbr.roughnessFactor = pbrNode["roughness_factor"].as<float>(material.pbr.roughnessFactor);
            material.pbr.normalScale = pbrNode["normal_scale"].as<float>(material.pbr.normalScale);
            material.pbr.occlusionStrength = pbrNode["occlusion_strength"].as<float>(material.pbr.occlusionStrength);
            material.pbr.emissiveIntensity = pbrNode["emissive_intensity"].as<float>(material.pbr.emissiveIntensity);
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
                cutoffFallback
            );
            const float opacityFallback = ClampMaterialAlphaValue(material.pbr.opacity, 1.0f);
            material.pbr.opacity = ClampMaterialAlphaValue(
                pbrNode["opacity"].as<float>(opacityFallback),
                opacityFallback
            );
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
