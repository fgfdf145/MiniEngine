#include "model_loader.h"

#include "gltf_model_loader.h"
#include "material_definition.h"

#include <log/log.h>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <system_error>

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
    pbr.alphaMode = material.alphaMode;
    pbr.alphaCutoff = ClampMaterialAlphaValue(material.alphaCutoff, 0.5f);
    pbr.opacity = ClampMaterialAlphaValue(material.opacity, 1.0f);
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
    material.alphaMode = pbr.alphaMode;
    material.alphaCutoff = ClampMaterialAlphaValue(pbr.alphaCutoff, 0.5f);
    material.opacity = ClampMaterialAlphaValue(pbr.opacity, 1.0f);
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

std::filesystem::path ModelLoader::CopyModelWithSortedReferences(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& targetDirectory)
{
    const std::string extension = ToLowerCopy(modelPath.extension().string());
    if (extension == ".gltf")
    {
        return GltfModelLoader::CopyWithSortedReferences(modelPath, targetDirectory);
    }

    // .glb (and anything else) is self-contained: plain copy into the folder.
    const std::filesystem::path dst = targetDirectory / modelPath.filename();
    std::error_code ec;
    std::filesystem::copy_file(modelPath, dst, std::filesystem::copy_options::skip_existing, ec);
    if (ec)
    {
        throw std::runtime_error(
            "Failed to copy '" + modelPath.string() + "' to '" + dst.string() + "': " + ec.message());
    }
    return dst;
}

LoadedModelData ModelLoader::LoadModel(const std::string& path, const ModelLoadProgressCallback& progress)
{
    const std::filesystem::path modelPath(path);
    if (!IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error(
            "Unsupported model format. MiniEngine only supports glTF 2.0 (*.gltf, *.glb): " + modelPath.string());
    }

    LoadedModelData modelData = GltfModelLoader::LoadModel(modelPath.string(), progress);
    for (ModelMaterialData& material : modelData.materials)
    {
        ApplyPbrSettings(material, BuildPbrSettingsFromMaterial(material));
    }
    for (size_t materialIndex = 0; materialIndex < modelData.materials.size(); ++materialIndex)
    {
        ModelMaterialData& rawMaterial = modelData.materials[materialIndex];
        ModelImportedMaterialInfo editable = BuildImportedMaterialInfo(rawMaterial);
        const std::filesystem::path sidecar =
            BuildMaterialDefinitionPath(modelPath, static_cast<uint32_t>(materialIndex));
        std::error_code probeError;
        const bool sidecarExists = std::filesystem::exists(sidecar, probeError);
        if (probeError)
        {
            LOG_WARN(
                "Unable to probe optional material sidecar '{}': {}; using imported glTF material",
                sidecar.string(),
                probeError.message());
            continue;
        }
        if (sidecarExists)
        {
            std::string warning;
            if (LoadMaterialDefinition(sidecar, editable, warning))
            {
                ApplyImportedMaterialInfo(editable, rawMaterial);
            }
            if (!warning.empty())
            {
                LOG_WARN("{}: {}", sidecar.string(), warning);
            }
        }
    }
    return modelData;
}
