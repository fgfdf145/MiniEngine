#include "model_loader.h"

#include "gltf_model_loader.h"
#include <algorithm>
#include <cctype>
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
    return modelData;
}
