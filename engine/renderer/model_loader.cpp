#include "model_loader.h"

#include <vulkan/fbx_model_loader.h>
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
    return ToLowerCopy(path.extension().string()) == ".fbx";
}

LoadedModelData ModelLoader::LoadModel(const std::string& path)
{
    const std::filesystem::path modelPath(path);
    if (!IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error(
            "Unsupported model format. MiniEngine only supports FBX (*.fbx): " + modelPath.string()
        );
    }

    LoadedModelData modelData = FbxModelLoader::LoadModel(modelPath.string());
    ApplyImportedAssetManifestOverrides(modelPath, modelData);
    return modelData;
}
