#include "model_loader.h"

#include "gltf_model_loader.h"
#include "material_graph_runtime.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace
{
std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::filesystem::path BuildImportedAssetManifestPath(const std::filesystem::path& modelPath)
{
    return std::filesystem::path(modelPath.string() + ".miniengine_asset.yaml");
}

MaterialPbrSurfaceSettings BuildPbrSettingsFromMaterial(const ModelMaterialData& material)
{
    MaterialPbrSurfaceSettings pbr{};
    for (size_t index = 0; index < 4; ++index)
    {
        pbr.baseColorFactor[index] = material.baseColor[index];
    }
    for (size_t index = 0; index < 3; ++index)
    {
        pbr.emissiveColor[index] = material.emissiveColor[index];
    }
    pbr.metallicFactor = material.metallicFactor;
    pbr.roughnessFactor = material.roughnessFactor;
    pbr.normalScale = material.normalScale;
    pbr.occlusionStrength = material.occlusionStrength;
    pbr.emissiveIntensity = material.emissiveIntensity;
    pbr.opacity = material.opacity;
    return pbr;
}

void ApplyPbrSettings(ModelMaterialData& material, const MaterialPbrSurfaceSettings& pbr)
{
    material.pbr = pbr;
    for (size_t index = 0; index < 4; ++index)
    {
        material.baseColor[index] = pbr.baseColorFactor[index];
    }
    for (size_t index = 0; index < 3; ++index)
    {
        material.emissiveColor[index] = pbr.emissiveColor[index];
    }
    material.metallicFactor = pbr.metallicFactor;
    material.roughnessFactor = pbr.roughnessFactor;
    material.normalScale = pbr.normalScale;
    material.occlusionStrength = pbr.occlusionStrength;
    material.emissiveIntensity = pbr.emissiveIntensity;
    material.opacity = pbr.opacity;
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

void ApplyImportedMaterialInfo(ModelMaterialData& material, const ModelImportedMaterialInfo& importedMaterial)
{
    material.name = importedMaterial.name;
    material.baseColorTexturePath = importedMaterial.baseColorTexturePath;
    material.normalTexturePath = importedMaterial.normalTexturePath;
    material.metallicTexturePath = importedMaterial.metallicTexturePath;
    material.roughnessTexturePath = importedMaterial.roughnessTexturePath;
    material.occlusionTexturePath = importedMaterial.occlusionTexturePath;
    material.emissiveTexturePath = importedMaterial.emissiveTexturePath;
    ApplyPbrSettings(material, importedMaterial.pbr);
    material.blendGraph = importedMaterial.blendGraph;
    material.shaderGraph = importedMaterial.shaderGraph;
}

void ApplyImportedMaterialNodeOverrides(const YAML::Node& materialNode, ModelImportedMaterialInfo& material)
{
    if (!materialNode || !materialNode.IsMap())
    {
        return;
    }

    material.name = materialNode["name"].as<std::string>(material.name);
    if (const YAML::Node value = materialNode["base_color_texture_path"]; value)
    {
        material.baseColorTexturePath = value.as<std::string>(material.baseColorTexturePath);
    }
    if (const YAML::Node value = materialNode["normal_texture_path"]; value)
    {
        material.normalTexturePath = value.as<std::string>(material.normalTexturePath);
    }
    if (const YAML::Node value = materialNode["metallic_texture_path"]; value)
    {
        material.metallicTexturePath = value.as<std::string>(material.metallicTexturePath);
    }
    if (const YAML::Node value = materialNode["roughness_texture_path"]; value)
    {
        material.roughnessTexturePath = value.as<std::string>(material.roughnessTexturePath);
    }
    if (const YAML::Node value = materialNode["occlusion_texture_path"]; value)
    {
        material.occlusionTexturePath = value.as<std::string>(material.occlusionTexturePath);
    }
    if (const YAML::Node value = materialNode["emissive_texture_path"]; value)
    {
        material.emissiveTexturePath = value.as<std::string>(material.emissiveTexturePath);
    }
    if (const YAML::Node pbrNode = materialNode["pbr"]; pbrNode && pbrNode.IsMap())
    {
        MaterialPbrSurfaceSettings pbr = material.pbr;
        if (pbrNode["base_color_factor"] && pbrNode["base_color_factor"].IsSequence() && pbrNode["base_color_factor"].size() == 4)
        {
            for (size_t index = 0; index < 4; ++index)
            {
                pbr.baseColorFactor[index] = pbrNode["base_color_factor"][index].as<float>(pbr.baseColorFactor[index]);
            }
        }
        if (pbrNode["emissive_color"] && pbrNode["emissive_color"].IsSequence() && pbrNode["emissive_color"].size() == 3)
        {
            for (size_t index = 0; index < 3; ++index)
            {
                pbr.emissiveColor[index] = pbrNode["emissive_color"][index].as<float>(pbr.emissiveColor[index]);
            }
        }
        pbr.metallicFactor = pbrNode["metallic_factor"].as<float>(pbr.metallicFactor);
        pbr.roughnessFactor = pbrNode["roughness_factor"].as<float>(pbr.roughnessFactor);
        pbr.normalScale = pbrNode["normal_scale"].as<float>(pbr.normalScale);
        pbr.occlusionStrength = pbrNode["occlusion_strength"].as<float>(pbr.occlusionStrength);
        pbr.emissiveIntensity = pbrNode["emissive_intensity"].as<float>(pbr.emissiveIntensity);
        pbr.opacity = pbrNode["opacity"].as<float>(pbr.opacity);
        material.pbr = pbr;
    }
    if (const YAML::Node graphNode = materialNode["texture_graph"]; graphNode && graphNode.IsMap())
    {
        material.blendGraph.enabled = graphNode["enabled"].as<bool>(material.blendGraph.enabled);
        material.blendGraph.blendFactor = graphNode["blend_factor"].as<float>(material.blendGraph.blendFactor);
        material.blendGraph.blendMaskTexturePath =
            graphNode["blend_mask_texture_path"].as<std::string>(material.blendGraph.blendMaskTexturePath);
        material.blendGraph.secondaryBaseColorTexturePath =
            graphNode["secondary_base_color_texture_path"].as<std::string>(material.blendGraph.secondaryBaseColorTexturePath);
        material.blendGraph.secondaryNormalTexturePath =
            graphNode["secondary_normal_texture_path"].as<std::string>(material.blendGraph.secondaryNormalTexturePath);
        material.blendGraph.secondaryMetallicTexturePath =
            graphNode["secondary_metallic_texture_path"].as<std::string>(material.blendGraph.secondaryMetallicTexturePath);
        material.blendGraph.secondaryRoughnessTexturePath =
            graphNode["secondary_roughness_texture_path"].as<std::string>(material.blendGraph.secondaryRoughnessTexturePath);
        material.blendGraph.secondaryOcclusionTexturePath =
            graphNode["secondary_occlusion_texture_path"].as<std::string>(material.blendGraph.secondaryOcclusionTexturePath);
        material.blendGraph.secondaryEmissiveTexturePath =
            graphNode["secondary_emissive_texture_path"].as<std::string>(material.blendGraph.secondaryEmissiveTexturePath);
    }

    DeserializeMaterialShaderGraph(materialNode["shader_graph"], material.name, std::nullopt, material);
    CompileMaterialShaderGraph(material);
}

bool TryLoadImportedMaterialAsset(
    const std::filesystem::path& modelPath,
    const YAML::Node& manifestMaterialNode,
    ModelImportedMaterialInfo& material
)
{
    if (!manifestMaterialNode || !manifestMaterialNode.IsMap() || !manifestMaterialNode["material_asset_path"])
    {
        return false;
    }

    std::filesystem::path materialAssetPath =
        manifestMaterialNode["material_asset_path"].as<std::string>(std::string{});
    if (materialAssetPath.empty())
    {
        return false;
    }
    if (!materialAssetPath.is_absolute())
    {
        materialAssetPath = (modelPath.parent_path() / materialAssetPath).lexically_normal();
    }

    if (!std::filesystem::exists(materialAssetPath))
    {
        return false;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(materialAssetPath.string());
        const YAML::Node materialNode = root["material"];
        if (!materialNode || !materialNode.IsMap())
        {
            return false;
        }

        ApplyImportedMaterialNodeOverrides(materialNode, material);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void ApplyImportedAssetManifestOverrides(const std::filesystem::path& modelPath, LoadedModelData& modelData)
{
    const std::filesystem::path manifestPath = BuildImportedAssetManifestPath(modelPath);
    if (!std::filesystem::exists(manifestPath))
    {
        return;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(manifestPath.string());
        const YAML::Node materialsNode = root["materials"];
        if (!materialsNode || !materialsNode.IsSequence())
        {
            return;
        }

        const size_t materialCount = std::min(materialsNode.size(), modelData.materials.size());
        for (size_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
        {
            const YAML::Node materialNode = materialsNode[materialIndex];
            if (!materialNode || !materialNode.IsMap())
            {
                continue;
            }

            ModelMaterialData& material = modelData.materials[materialIndex];
            ApplyPbrSettings(material, BuildPbrSettingsFromMaterial(material));
            ModelImportedMaterialInfo importedMaterial = BuildImportedMaterialInfo(material);
            if (!TryLoadImportedMaterialAsset(modelPath, materialNode, importedMaterial))
            {
                ApplyImportedMaterialNodeOverrides(materialNode, importedMaterial);
            }
            ApplyImportedMaterialInfo(material, importedMaterial);
        }
    }
    catch (...)
    {
        // Ignore invalid sidecar metadata and keep the model's original bindings.
    }
}
}

bool ModelLoader::IsSupportedModelPath(const std::filesystem::path& path)
{
    const std::string extension = ToLowerCopy(path.extension().string());
    return extension == ".gltf" || extension == ".glb";
}

bool ModelLoader::IsImportAvailable()
{
    return true;
}

const char* ModelLoader::GetImporterName()
{
    return "tinygltf";
}

LoadedModelData ModelLoader::LoadModel(const std::string& path)
{
    const std::filesystem::path modelPath(path);
    if (!IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error(
            "Unsupported model format. MiniEngine only supports glTF 2.0 (*.gltf, *.glb): " + modelPath.string()
        );
    }

    LoadedModelData modelData = GltfModelLoader::LoadModel(modelPath.string());
    for (ModelMaterialData& material : modelData.materials)
    {
        ApplyPbrSettings(material, BuildPbrSettingsFromMaterial(material));
    }
    ApplyImportedAssetManifestOverrides(modelPath, modelData);
    return modelData;
}
