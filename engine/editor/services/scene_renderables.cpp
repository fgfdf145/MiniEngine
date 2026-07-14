#include "scene_renderables.h"

#include <renderer_shared_state.h>

#include <mesh.h>
#include <model_cache.h>
#include <model_loader.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
MaterialPushConstants BuildDefaultMaterialForTag(const std::string& tagName)
{
    MaterialPushConstants material{};
    if (tagName == "Cube A")
    {
        material.baseColorFactor[0] = 1.0f;
        material.baseColorFactor[1] = 0.55f;
        material.baseColorFactor[2] = 0.35f;
    }
    else if (tagName == "Cube B")
    {
        material.baseColorFactor[0] = 0.35f;
        material.baseColorFactor[1] = 0.75f;
        material.baseColorFactor[2] = 1.0f;
    }
    return material;
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

ModelImportedSubmeshInfo BuildImportedSubmeshInfo(const ModelSubmeshData& submesh)
{
    return ModelImportedSubmeshInfo{
        submesh.name,
        static_cast<uint32_t>(submesh.mesh.vertices.size()),
        static_cast<uint32_t>(submesh.mesh.indices.size()),
        submesh.materialIndex,
        submesh.hasTexCoords,
        submesh.hasNormals,
        submesh.hasTangents
    };
}
}

void RebuildSceneRenderables(RendererSharedState& state)
{
    std::vector<CpuRenderSubmesh> newRenderSubmeshes;

    state.GetEditorWorld().ForEachEntity([&](
        entt::entity entity,
        const TagComponent& tag,
        const TransformComponent&,
        const ModelComponent& model
    )
    {
        if (model.sourcePath.empty())
        {
            state.GetEditorWorld().UpdateModelInfo(
                entity,
                tag.name,
                std::string{},
                1,
                WorldUnits::kDefaultCubeMinBoundsMeters,
                WorldUnits::kDefaultCubeMaxBoundsMeters,
                true,
                {},
                {}
            );

            CpuRenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.mesh = CreateDefaultCubeMesh();
            renderSubmesh.material = BuildDefaultMaterialForTag(tag.name);
            renderSubmesh.hasTexCoords = true;
            renderSubmesh.name = tag.name;
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            return;
        }

        std::shared_ptr<LoadedModelData> modelDataPtr = ModelCache::Get(model.sourcePath);
        if (!modelDataPtr)
        {
            // Don't do a synchronous load while an async loader is running on another thread:
            // the model loader is not thread-safe and concurrent access to the same file crashes.
            // ProcessPendingOperations will call RebuildSceneRenderables() again once the
            // async load completes and the data is in the cache.
            if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
            {
                return;
            }
            modelDataPtr = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(model.sourcePath));
            ModelCache::Store(model.sourcePath, modelDataPtr);
        }
        const LoadedModelData& modelData = *modelDataPtr;

        // Resolve texture paths relative to the model file's current directory.
        // This makes asset packages portable: moving model + textures together keeps
        // all relative references valid.
        const std::filesystem::path modelDir =
            std::filesystem::path(model.sourcePath).parent_path();
        const auto resolveTex = [&modelDir](const std::string& p) -> std::string
        {
            if (p.empty() || std::filesystem::path(p).is_absolute())
            {
                return p;
            }
            return (modelDir / p).lexically_normal().string();
        };

        std::vector<ModelImportedMaterialInfo> importedMaterials;
        importedMaterials.reserve(modelData.materials.size());
        for (const ModelMaterialData& rawMat : modelData.materials)
        {
            ModelMaterialData mat = rawMat;
            mat.baseColorTexturePath  = resolveTex(rawMat.baseColorTexturePath);
            mat.normalTexturePath     = resolveTex(rawMat.normalTexturePath);
            mat.metallicTexturePath   = resolveTex(rawMat.metallicTexturePath);
            mat.roughnessTexturePath  = resolveTex(rawMat.roughnessTexturePath);
            mat.occlusionTexturePath  = resolveTex(rawMat.occlusionTexturePath);
            mat.emissiveTexturePath   = resolveTex(rawMat.emissiveTexturePath);
            importedMaterials.push_back(BuildImportedMaterialInfo(mat));
        }

        std::vector<ModelImportedSubmeshInfo> importedSubmeshes;
        importedSubmeshes.reserve(modelData.submeshes.size());
        std::vector<bool> materialUsesUv(importedMaterials.size(), false);
        for (const ModelSubmeshData& submesh : modelData.submeshes)
        {
            importedSubmeshes.push_back(BuildImportedSubmeshInfo(submesh));
            if (submesh.hasTexCoords && submesh.materialIndex < materialUsesUv.size())
            {
                materialUsesUv[submesh.materialIndex] = true;
            }
        }
        if (!model.baseColorTextureOverridePath.empty())
        {
            for (size_t materialIndex = 0; materialIndex < importedMaterials.size(); ++materialIndex)
            {
                if (materialUsesUv[materialIndex])
                {
                    importedMaterials[materialIndex].baseColorTexturePath = model.baseColorTextureOverridePath;
                }
            }
        }

        for (const ModelSubmeshData& submesh : modelData.submeshes)
        {
            CpuRenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.mesh = submesh.mesh;
            renderSubmesh.hasTexCoords = submesh.hasTexCoords;

            const ModelMaterialData& material = modelData.materials[submesh.materialIndex];
            renderSubmesh.doubleSided = material.doubleSided;
            renderSubmesh.material.baseColorFactor[0] = material.baseColor[0];
            renderSubmesh.material.baseColorFactor[1] = material.baseColor[1];
            renderSubmesh.material.baseColorFactor[2] = material.baseColor[2];
            renderSubmesh.material.baseColorFactor[3] = material.baseColor[3] * material.opacity;
            renderSubmesh.material.emissiveFactor[0] = material.emissiveColor[0] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[1] = material.emissiveColor[1] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[2] = material.emissiveColor[2] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[3] = material.alphaCutoff;
            renderSubmesh.material.surfaceFactors[0] = material.metallicFactor;
            renderSubmesh.material.surfaceFactors[1] = material.roughnessFactor;
            renderSubmesh.material.surfaceFactors[2] = material.normalScale;
            renderSubmesh.material.surfaceFactors[3] = material.occlusionStrength;
            renderSubmesh.material.nodeGraphFactors[0] = material.blendGraph.enabled ? 1.0f : 0.0f;
            renderSubmesh.material.nodeGraphFactors[1] = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);
            renderSubmesh.material.nodeGraphFactors[2] = 1.0f;
            renderSubmesh.material.nodeGraphFactors[3] = 0.0f;
            renderSubmesh.name = submesh.name;
            if (submesh.hasTexCoords)
            {
                renderSubmesh.textures.baseColor =
                    model.baseColorTextureOverridePath.empty()
                    ? resolveTex(material.baseColorTexturePath)
                    : model.baseColorTextureOverridePath;
                renderSubmesh.textures.normal    = resolveTex(material.normalTexturePath);
                renderSubmesh.textures.metallic  = resolveTex(material.metallicTexturePath);
                renderSubmesh.textures.roughness = resolveTex(material.roughnessTexturePath);
                renderSubmesh.textures.occlusion = resolveTex(material.occlusionTexturePath);
                renderSubmesh.textures.emissive  = resolveTex(material.emissiveTexturePath);
                renderSubmesh.textures.secondaryBaseColor =
                    material.blendGraph.secondaryBaseColorTexturePath.empty()
                    ? renderSubmesh.textures.baseColor
                    : material.blendGraph.secondaryBaseColorTexturePath;
                renderSubmesh.textures.secondaryNormal =
                    material.blendGraph.secondaryNormalTexturePath.empty()
                    ? renderSubmesh.textures.normal
                    : material.blendGraph.secondaryNormalTexturePath;
                renderSubmesh.textures.secondaryMetallic =
                    material.blendGraph.secondaryMetallicTexturePath.empty()
                    ? renderSubmesh.textures.metallic
                    : material.blendGraph.secondaryMetallicTexturePath;
                renderSubmesh.textures.secondaryRoughness =
                    material.blendGraph.secondaryRoughnessTexturePath.empty()
                    ? renderSubmesh.textures.roughness
                    : material.blendGraph.secondaryRoughnessTexturePath;
                renderSubmesh.textures.secondaryOcclusion =
                    material.blendGraph.secondaryOcclusionTexturePath.empty()
                    ? renderSubmesh.textures.occlusion
                    : material.blendGraph.secondaryOcclusionTexturePath;
                renderSubmesh.textures.secondaryEmissive =
                    material.blendGraph.secondaryEmissiveTexturePath.empty()
                    ? renderSubmesh.textures.emissive
                    : material.blendGraph.secondaryEmissiveTexturePath;
                renderSubmesh.textures.blendMask = material.blendGraph.blendMaskTexturePath;
            }
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
        }

        state.GetEditorWorld().UpdateModelInfo(
            entity,
            model.displayName,
            model.sourcePath,
            static_cast<uint32_t>(modelData.submeshes.size()),
            modelData.minBounds,
            modelData.maxBounds,
            modelData.hasBounds,
            importedMaterials,
            importedSubmeshes
        );
    });

    state.rendererWorld.SetRenderSubmeshes(std::move(newRenderSubmeshes));
    state.renderablesDirty = true;
}

