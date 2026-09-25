#include "model_loader.h"

#include "gltf_model_loader.h"
#include "material_definition.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace me
{

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
    pbr.clearcoatFactor = material.clearcoatFactor;
    pbr.clearcoatRoughnessFactor = material.clearcoatRoughnessFactor;
    for (size_t index = 0; index < 3; ++index)
    {
        pbr.sheenColorFactor[index] = material.sheenColorFactor[index];
    }
    pbr.sheenRoughnessFactor = material.sheenRoughnessFactor;
    pbr.anisotropyStrength = material.anisotropyStrength;
    pbr.anisotropyRotation = material.anisotropyRotation;
    pbr.ior = material.ior;
    pbr.specularFactor = material.specularFactor;
    for (size_t index = 0; index < 3; ++index)
    {
        pbr.specularColorFactor[index] = material.specularColorFactor[index];
    }
    pbr.clearcoatNormalScale = material.clearcoatNormalScale;
    pbr.iridescenceFactor = material.iridescenceFactor;
    pbr.iridescenceIor = material.iridescenceIor;
    pbr.iridescenceThicknessMinimum = material.iridescenceThicknessMinimum;
    pbr.iridescenceThicknessMaximum = material.iridescenceThicknessMaximum;
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
    material.clearcoatFactor = pbr.clearcoatFactor;
    material.clearcoatRoughnessFactor = pbr.clearcoatRoughnessFactor;
    for (size_t index = 0; index < 3; ++index)
    {
        material.sheenColorFactor[index] = pbr.sheenColorFactor[index];
    }
    material.sheenRoughnessFactor = pbr.sheenRoughnessFactor;
    material.anisotropyStrength = pbr.anisotropyStrength;
    material.anisotropyRotation = pbr.anisotropyRotation;
    material.ior = pbr.ior;
    material.specularFactor = pbr.specularFactor;
    for (size_t index = 0; index < 3; ++index)
    {
        material.specularColorFactor[index] = pbr.specularColorFactor[index];
    }
    material.clearcoatNormalScale = pbr.clearcoatNormalScale;
    material.iridescenceFactor = pbr.iridescenceFactor;
    material.iridescenceIor = pbr.iridescenceIor;
    material.iridescenceThicknessMinimum = pbr.iridescenceThicknessMinimum;
    material.iridescenceThicknessMaximum = pbr.iridescenceThicknessMaximum;
}

struct MaterialDefinitionFile
{
    std::filesystem::path path;
    std::optional<std::string> materialName; // nullopt: saved before names were checked
};

// Material definitions are named by index, but a glTF edited or replaced
// outside the editor can reorder its materials. Each definition records the
// name of the material it was saved for, so it is applied only to a material
// of that name: at its own index when the names agree, otherwise to the one
// material that carries the name. A definition that fits nowhere is skipped
// rather than dressing the wrong material.
void ApplyMaterialDefinitions(const std::filesystem::path& modelPath, LoadedModelData& modelData)
{
    std::unordered_map<uint32_t, MaterialDefinitionFile> definitions;
    for (const std::filesystem::path& file : FindMaterialDefinitionFiles(modelPath))
    {
        if (const std::optional<uint32_t> index = MaterialDefinitionIndex(modelPath, file))
        {
            definitions[*index] = MaterialDefinitionFile{file, ReadMaterialDefinitionName(file)};
        }
    }
    if (definitions.empty())
    {
        return;
    }

    std::unordered_map<std::string, size_t> materialNameCounts;
    for (const ModelMaterialData& material : modelData.materials)
    {
        ++materialNameCounts[material.name];
    }

    for (size_t materialIndex = 0; materialIndex < modelData.materials.size(); ++materialIndex)
    {
        ModelMaterialData& rawMaterial = modelData.materials[materialIndex];
        const MaterialDefinitionFile* chosen = nullptr;

        const auto own = definitions.find(static_cast<uint32_t>(materialIndex));
        if (own != definitions.end() &&
            (!own->second.materialName.has_value() || *own->second.materialName == rawMaterial.name))
        {
            chosen = &own->second;
        }
        else if (!rawMaterial.name.empty() && materialNameCounts[rawMaterial.name] == 1)
        {
            for (const auto& [index, definition] : definitions)
            {
                if (definition.materialName == rawMaterial.name)
                {
                    chosen = &definition;
                    break;
                }
            }
        }

        if (chosen == nullptr)
        {
            if (own != definitions.end())
            {
                LOG_WARN(
                    "'{}' was saved for material '{}', but material {} is '{}'; not applied",
                    own->second.path.string(),
                    own->second.materialName.value_or(""),
                    materialIndex,
                    rawMaterial.name);
            }
            continue;
        }

        ModelImportedMaterialInfo editable = BuildImportedMaterialInfo(rawMaterial);
        std::string warning;
        if (LoadMaterialDefinition(chosen->path, editable, warning))
        {
            ApplyImportedMaterialInfo(editable, rawMaterial);
        }
        if (!warning.empty())
        {
            LOG_WARN("{}: {}", chosen->path.string(), warning);
        }
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

std::filesystem::path ModelLoader::CopyModelWithSortedReferences(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& targetDirectory)
{
    const std::string extension = ToLowerCopy(modelPath.extension().string());

    std::filesystem::path dst;
    if (extension == ".gltf")
    {
        dst = GltfModelLoader::CopyWithSortedReferences(modelPath, targetDirectory);
    }
    else
    {
        // .glb (and anything else) is self-contained: plain copy into the folder.
        dst = targetDirectory / modelPath.filename();
        std::error_code ec;
        // copy_options::none fails on an existing file: the caller resolves
        // conflicts before copying, and a silent skip would report the old
        // file as the new import.
        std::filesystem::copy_file(modelPath, dst, std::filesystem::copy_options::none, ec);
        if (ec)
        {
            throw std::runtime_error(
                "Failed to copy '" + modelPath.string() + "' to '" + dst.string() + "': " + ec.message());
        }
    }

    // Images whose pixels live inside the model file become real files in the
    // bundle, so every texture path the loader produces is model-relative and
    // loading never has to write anything.
    GltfModelLoader::UnpackEmbeddedTextures(dst);
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
    ApplyMaterialDefinitions(modelPath, modelData);
    return modelData;
}
}
