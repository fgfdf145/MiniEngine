#include "model_import_service.h"

#include "scene_renderables.h"

#include <renderer_shared_state.h>

#include <log/log.h>
#include <material_graph_runtime.h>
#include <model_cache.h>
#include <model_loader.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{
// Copies user-edited material data (blendGraph, shaderGraph, pbr, texture paths) from an
// ImportedMaterialInfo into the raw ModelMaterialData held by the model cache.
// ModelMaterialData has redundant flat fields alongside a pbr struct; both must stay in sync.
void ApplyImportedMaterialToModelData(const ModelImportedMaterialInfo& src, ModelMaterialData& dst)
{
    dst.name                  = src.name;
    dst.baseColorTexturePath  = src.baseColorTexturePath;
    dst.normalTexturePath     = src.normalTexturePath;
    dst.metallicTexturePath   = src.metallicTexturePath;
    dst.roughnessTexturePath  = src.roughnessTexturePath;
    dst.occlusionTexturePath  = src.occlusionTexturePath;
    dst.emissiveTexturePath   = src.emissiveTexturePath;
    dst.pbr                   = src.pbr;
    dst.blendGraph            = src.blendGraph;
    dst.shaderGraph           = src.shaderGraph;
    // Mirror pbr fields into the flat copies used by RebuildSceneRenderables.
    dst.baseColor[0]      = src.pbr.baseColorFactor[0];
    dst.baseColor[1]      = src.pbr.baseColorFactor[1];
    dst.baseColor[2]      = src.pbr.baseColorFactor[2];
    dst.baseColor[3]      = src.pbr.baseColorFactor[3];
    dst.emissiveColor[0]  = src.pbr.emissiveColor[0];
    dst.emissiveColor[1]  = src.pbr.emissiveColor[1];
    dst.emissiveColor[2]  = src.pbr.emissiveColor[2];
    dst.metallicFactor    = src.pbr.metallicFactor;
    dst.roughnessFactor   = src.pbr.roughnessFactor;
    dst.normalScale       = src.pbr.normalScale;
    dst.occlusionStrength = src.pbr.occlusionStrength;
    dst.emissiveIntensity = src.pbr.emissiveIntensity;
    dst.opacity           = src.pbr.opacity;
}

// Serializes one material to a YAML node under a "material:" root map, with
// name/texture paths, a "pbr:" block, an optional "texture_graph:" blend block,
// and an optional "shader_graph:" block.
YAML::Node SerializeMaterialToYaml(const ModelImportedMaterialInfo& material)
{
    YAML::Node node(YAML::NodeType::Map);
    node["name"]                   = material.name;
    node["base_color_texture_path"]= material.baseColorTexturePath;
    node["normal_texture_path"]    = material.normalTexturePath;
    node["metallic_texture_path"]  = material.metallicTexturePath;
    node["roughness_texture_path"] = material.roughnessTexturePath;
    node["occlusion_texture_path"] = material.occlusionTexturePath;
    node["emissive_texture_path"]  = material.emissiveTexturePath;

    YAML::Node pbr(YAML::NodeType::Map);
    YAML::Node bcf(YAML::NodeType::Sequence);
    for (float v : material.pbr.baseColorFactor) bcf.push_back(v);
    pbr["base_color_factor"] = bcf;
    YAML::Node ec(YAML::NodeType::Sequence);
    for (float v : material.pbr.emissiveColor) ec.push_back(v);
    pbr["emissive_color"]      = ec;
    pbr["metallic_factor"]     = material.pbr.metallicFactor;
    pbr["roughness_factor"]    = material.pbr.roughnessFactor;
    pbr["normal_scale"]        = material.pbr.normalScale;
    pbr["occlusion_strength"]  = material.pbr.occlusionStrength;
    pbr["emissive_intensity"]  = material.pbr.emissiveIntensity;
    pbr["opacity"]             = material.pbr.opacity;
    node["pbr"] = pbr;

    const MaterialTextureBlendGraph& bg = material.blendGraph;
    const bool hasBlendData =
        bg.enabled ||
        !bg.blendMaskTexturePath.empty() ||
        !bg.secondaryBaseColorTexturePath.empty() ||
        !bg.secondaryNormalTexturePath.empty() ||
        !bg.secondaryMetallicTexturePath.empty() ||
        !bg.secondaryRoughnessTexturePath.empty() ||
        !bg.secondaryOcclusionTexturePath.empty() ||
        !bg.secondaryEmissiveTexturePath.empty();
    if (hasBlendData)
    {
        YAML::Node graph(YAML::NodeType::Map);
        graph["enabled"]                          = bg.enabled;
        graph["blend_factor"]                     = bg.blendFactor;
        graph["blend_mask_texture_path"]          = bg.blendMaskTexturePath;
        graph["secondary_base_color_texture_path"]= bg.secondaryBaseColorTexturePath;
        graph["secondary_normal_texture_path"]    = bg.secondaryNormalTexturePath;
        graph["secondary_metallic_texture_path"]  = bg.secondaryMetallicTexturePath;
        graph["secondary_roughness_texture_path"] = bg.secondaryRoughnessTexturePath;
        graph["secondary_occlusion_texture_path"] = bg.secondaryOcclusionTexturePath;
        graph["secondary_emissive_texture_path"]  = bg.secondaryEmissiveTexturePath;
        node["texture_graph"] = graph;
    }

    if (!material.shaderGraph.IsEmpty())
    {
        node["shader_graph"] = SerializeMaterialShaderGraph(material.shaderGraph);
    }

    return node;
}

// Writes a single material's YAML to disk. Returns the output path on success.
std::optional<std::filesystem::path> WriteMaterialYamlFile(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material
)
{
    const std::filesystem::path outPath =
        modelPath.parent_path() /
        (modelPath.stem().string() + "_" + std::to_string(materialIndex) + ".material.yaml");

    YAML::Node root(YAML::NodeType::Map);
    root["material"] = SerializeMaterialToYaml(material);

    std::ofstream outFile(outPath);
    if (!outFile)
    {
        LOG_ERROR("Failed to open material file for writing: '{}'", outPath.string());
        return std::nullopt;
    }
    outFile << root;
    return outPath;
}
}

namespace ModelImportService
{
std::string ImportModelIntoAssetDirectory(const std::string& sourcePath, const std::string& destinationDirectory)
{
    const std::filesystem::path src = std::filesystem::path(sourcePath);
    if (!std::filesystem::exists(src))
    {
        throw std::runtime_error("Source file does not exist: " + sourcePath);
    }

    const std::filesystem::path dstDir = std::filesystem::path(destinationDirectory);
    const std::filesystem::path dst = dstDir / src.filename();

    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::skip_existing, ec);
    if (ec)
    {
        throw std::runtime_error("Failed to import '" + src.string() + "': " + ec.message());
    }

    // For .gltf (ASCII), also copy companion .bin and texture files from the same directory.
    const std::string ext = [&src]()
    {
        std::string e = src.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return e;
    }();

    if (ext == ".gltf")
    {
        // Recursively copy all texture / binary companion files from the source
        // directory tree into the same relative sub-path under dstDir.
        // This preserves the relative layout that the .gltf URIs rely on, so
        // moving the imported asset folder as a unit keeps all references valid.
        static constexpr std::array<std::string_view, 8> kAssetExts = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds", ".bin"
        };

        const std::filesystem::path srcDir = src.parent_path();
        for (const auto& item : std::filesystem::recursive_directory_iterator(srcDir, ec))
        {
            if (ec)
            {
                break;
            }
            if (!item.is_regular_file(ec) || ec)
            {
                continue;
            }

            std::string itemExt = item.path().extension().string();
            std::transform(itemExt.begin(), itemExt.end(), itemExt.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

            const bool isAsset = std::any_of(
                kAssetExts.begin(), kAssetExts.end(),
                [&itemExt](std::string_view e){ return itemExt == e; }
            );
            if (!isAsset)
            {
                continue;
            }

            // Compute the relative path from the source model directory and
            // mirror it under the destination directory.
            const std::filesystem::path relPath =
                item.path().lexically_relative(srcDir);
            const std::filesystem::path companionDst = dstDir / relPath;

            std::error_code mkdirEc;
            std::filesystem::create_directories(companionDst.parent_path(), mkdirEc);

            std::error_code copyEc;
            std::filesystem::copy_file(item.path(), companionDst,
                std::filesystem::copy_options::skip_existing, copyEc);
            if (copyEc)
            {
                LOG_WARN("Could not copy companion file '{}': {}",
                    item.path().string(), copyEc.message());
            }
            else
            {
                LOG_INFO("Copied companion: {} -> {}", item.path().string(), companionDst.string());
            }
        }
    }

    LOG_INFO("Imported model '{}' -> '{}'", src.string(), dst.string());
    return dst.string();
}

void DeleteAssetPath(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path), ec);
    if (ec)
    {
        throw std::runtime_error("Failed to delete '" + path + "': " + ec.message());
    }
    LOG_INFO("Deleted asset: {}", path);
}

void PasteAsset(const std::string& sourcePath, const std::string& destinationDirectory)
{
    const std::filesystem::path src = std::filesystem::path(sourcePath);
    const std::filesystem::path dst = std::filesystem::path(destinationDirectory) / src.filename();
    std::error_code eqEc;
    if (std::filesystem::equivalent(src, dst, eqEc) && !eqEc)
    {
        return;
    }
    std::error_code ec;
    std::filesystem::copy(src, dst,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
        ec);
    if (ec)
    {
        throw std::runtime_error(
            "Failed to copy '" + sourcePath + "' to '" + destinationDirectory + "': " + ec.message()
        );
    }
    LOG_INFO("Copied asset '{}' -> '{}'", sourcePath, dst.string());
}

void UpdateImportedMaterialDefinition(
    RendererSharedState& state,
    const std::string& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material
)
{
    if (modelPath.empty())
    {
        return;
    }

    // Update the single material at the given index in the model cache.
    std::shared_ptr<LoadedModelData> cached = ModelCache::Get(modelPath);
    if (cached && materialIndex < cached->materials.size())
    {
        ApplyImportedMaterialToModelData(material, cached->materials[materialIndex]);
    }

    WriteMaterialYamlFile(
        std::filesystem::path(modelPath),
        materialIndex,
        material
    );

    RebuildSceneRenderables(state);
    state.renderablesDirty = true;
    LOG_INFO(
        "Updated material {} for model '{}'",
        materialIndex,
        modelPath
    );
}

void UpdateImportedModelMaterialDefinitions(
    RendererSharedState& state,
    const std::string& modelPathString,
    const std::vector<ModelImportedMaterialInfo>& materials
)
{
    if (modelPathString.empty() || materials.empty())
    {
        return;
    }

    const std::filesystem::path modelPath(modelPathString);

    // Propagate user edits into the cached raw model data so that
    // RebuildSceneRenderables picks up the new blend graphs and pbr factors.
    std::shared_ptr<LoadedModelData> cached = ModelCache::Get(modelPathString);
    if (cached)
    {
        const size_t count = std::min(materials.size(), cached->materials.size());
        for (size_t i = 0; i < count; ++i)
        {
            ApplyImportedMaterialToModelData(materials[i], cached->materials[i]);
        }
    }

    // Persist each material as a sidecar .material.yaml file alongside the model.
    for (size_t i = 0; i < materials.size(); ++i)
    {
        const auto outPath = WriteMaterialYamlFile(modelPath, static_cast<uint32_t>(i), materials[i]);
        if (outPath.has_value())
        {
            LOG_INFO("Saved material '{}' -> '{}'", materials[i].name, outPath->string());
        }
    }

    RebuildSceneRenderables(state);
    state.renderablesDirty = true;
    LOG_INFO(
        "Saved {} material(s) for model '{}'",
        materials.size(),
        modelPathString
    );
}

}
