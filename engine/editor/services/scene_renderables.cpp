#include "scene_renderables.h"

#include <engine/editor/renderer_shared_state.h>

#include <engine/asset/mesh.h>
#include <engine/asset/material_definition.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/model_loader.h>
#include <engine/core/log/log.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace me
{

namespace
{
GpuMaterialData BuildDefaultMaterialForTag(const std::string& tagName)
{
    GpuMaterialData material{};
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

ModelImportedSubmeshInfo BuildImportedSubmeshInfo(const ModelSubmeshData& submesh)
{
    return ModelImportedSubmeshInfo{
        submesh.name,
        static_cast<uint32_t>(submesh.mesh.vertices.size()),
        static_cast<uint32_t>(submesh.mesh.indices.size()),
        submesh.materialIndex,
        submesh.hasTexCoords,
        submesh.hasNormals,
        submesh.hasTangents};
}

std::vector<CpuRenderSubmesh> BuildEntityRenderSubmeshes(RendererSharedState& state, entt::entity entity)
{
    IEditorWorld& world = state.GetEditorWorld();
    const TagComponent& tag = world.GetTag(entity);
    const ModelComponent& model = world.GetModel(entity);
    std::vector<CpuRenderSubmesh> renderSubmeshes;

    if (model.sourcePath.empty())
    {
        world.UpdateModelInfo(
            entity,
            tag.name,
            std::string{},
            1,
            WorldUnits::kDefaultCubeMinBoundsMeters,
            WorldUnits::kDefaultCubeMaxBoundsMeters,
            true,
            {},
            {},
            {},
            0);

        const ModelMaterialData material{};
        CpuRenderSubmesh renderSubmesh{};
        renderSubmesh.entity = entity;
        renderSubmesh.mesh = std::make_shared<const MeshData>(CreateDefaultCubeMesh());
        renderSubmesh.material = BuildDefaultMaterialForTag(tag.name);
        renderSubmesh.alphaMode = material.alphaMode;
        const MeshBounds bounds = ComputeMeshBounds(*renderSubmesh.mesh);
        renderSubmesh.localBoundsCenter = bounds.center;
        renderSubmesh.localBoundsRadius = bounds.radius;
        renderSubmesh.material.alphaCutoff =
            ClampMaterialAlphaValue(material.alphaCutoff, 0.5f);
        renderSubmesh.hasTexCoords = true;
        renderSubmesh.name = tag.name;
        renderSubmeshes.push_back(std::move(renderSubmesh));
        return renderSubmeshes;
    }

    std::shared_ptr<const LoadedModelData> modelDataPtr = ModelCache::Get(model.sourcePath);
    if (!modelDataPtr)
    {
        // Don't do a synchronous load while an async loader is running on another thread:
        // the model loader is not thread-safe and concurrent access to the same file crashes.
        if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
        {
            return renderSubmeshes;
        }
        auto loaded = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(model.sourcePath));
        ModelCache::Store(model.sourcePath, loaded);
        modelDataPtr = loaded;
    }
    const LoadedModelData& modelData = *modelDataPtr;

    const std::filesystem::path modelDir = std::filesystem::path(model.sourcePath).parent_path();
    const auto resolveTex = [&modelDir](const std::string& path) -> std::string
    {
        if (path.empty() || std::filesystem::path(path).is_absolute())
        {
            return path;
        }
        return (modelDir / path).lexically_normal().string();
    };

    std::vector<ModelImportedMaterialInfo> importedMaterials;
    importedMaterials.reserve(modelData.materials.size());
    for (const ModelMaterialData& rawMaterial : modelData.materials)
    {
        ModelMaterialData material = rawMaterial;
        material.baseColorTexturePath = resolveTex(rawMaterial.baseColorTexturePath);
        material.normalTexturePath = resolveTex(rawMaterial.normalTexturePath);
        material.metallicTexturePath = resolveTex(rawMaterial.metallicTexturePath);
        material.roughnessTexturePath = resolveTex(rawMaterial.roughnessTexturePath);
        material.occlusionTexturePath = resolveTex(rawMaterial.occlusionTexturePath);
        material.emissiveTexturePath = resolveTex(rawMaterial.emissiveTexturePath);
        material.clearcoatTexturePath = resolveTex(rawMaterial.clearcoatTexturePath);
        material.clearcoatRoughnessTexturePath = resolveTex(rawMaterial.clearcoatRoughnessTexturePath);
        material.sheenColorTexturePath = resolveTex(rawMaterial.sheenColorTexturePath);
        material.sheenRoughnessTexturePath = resolveTex(rawMaterial.sheenRoughnessTexturePath);
        material.anisotropyTexturePath = resolveTex(rawMaterial.anisotropyTexturePath);
        material.specularTexturePath = resolveTex(rawMaterial.specularTexturePath);
        material.specularColorTexturePath = resolveTex(rawMaterial.specularColorTexturePath);
        material.clearcoatNormalTexturePath = resolveTex(rawMaterial.clearcoatNormalTexturePath);
        material.iridescenceTexturePath = resolveTex(rawMaterial.iridescenceTexturePath);
        material.iridescenceThicknessTexturePath = resolveTex(rawMaterial.iridescenceThicknessTexturePath);
        material.transmissionTexturePath = resolveTex(rawMaterial.transmissionTexturePath);
        material.thicknessTexturePath = resolveTex(rawMaterial.thicknessTexturePath);
        material.diffuseTransmissionTexturePath = resolveTex(rawMaterial.diffuseTransmissionTexturePath);
        material.diffuseTransmissionColorTexturePath = resolveTex(rawMaterial.diffuseTransmissionColorTexturePath);
        importedMaterials.push_back(BuildImportedMaterialInfo(material));
    }

    // The entity's material variant; a name the model does not have falls back to the default.
    const std::optional<uint32_t> variantIndex = FindMaterialVariant(modelData, model.materialVariant);
    if (!model.materialVariant.empty() && !variantIndex.has_value())
    {
        LOG_WARN(
            "'{}' has no material variant '{}'; using its default materials",
            model.sourcePath,
            model.materialVariant);
    }

    std::vector<ModelImportedSubmeshInfo> importedSubmeshes;
    importedSubmeshes.reserve(modelData.submeshes.size());
    std::vector<bool> materialUsesUv(importedMaterials.size(), false);
    for (const ModelSubmeshData& submesh : modelData.submeshes)
    {
        importedSubmeshes.push_back(BuildImportedSubmeshInfo(submesh));
        const uint32_t materialIndex = ResolveSubmeshMaterialIndex(submesh, variantIndex);
        if (submesh.hasTexCoords && materialIndex < materialUsesUv.size())
        {
            materialUsesUv[materialIndex] = true;
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

    renderSubmeshes.reserve(modelData.submeshes.size());
    for (const ModelSubmeshData& submesh : modelData.submeshes)
    {
        CpuRenderSubmesh renderSubmesh{};
        renderSubmesh.entity = entity;
        // Aliasing shared_ptr: aims at this submesh's mesh while sharing ownership of the
        // cached model it lives in, so the geometry is never copied out of the cache.
        renderSubmesh.mesh = std::shared_ptr<const MeshData>(modelDataPtr, &submesh.mesh);
        renderSubmesh.hasTexCoords = submesh.hasTexCoords;

        const ModelMaterialData& material = modelData.materials[ResolveSubmeshMaterialIndex(submesh, variantIndex)];
        renderSubmesh.doubleSided = material.doubleSided;
        renderSubmesh.alphaMode = material.alphaMode;
        renderSubmesh.localBoundsCenter = submesh.boundsCenter;
        renderSubmesh.localBoundsRadius = submesh.boundsRadius;
        renderSubmesh.material.baseColorFactor[0] = material.baseColor[0];
        renderSubmesh.material.baseColorFactor[1] = material.baseColor[1];
        renderSubmesh.material.baseColorFactor[2] = material.baseColor[2];
        renderSubmesh.material.baseColorFactor[3] = material.baseColor[3] * material.opacity;
        renderSubmesh.material.emissiveFactor[0] = material.emissiveColor[0] * material.emissiveIntensity;
        renderSubmesh.material.emissiveFactor[1] = material.emissiveColor[1] * material.emissiveIntensity;
        renderSubmesh.material.emissiveFactor[2] = material.emissiveColor[2] * material.emissiveIntensity;
        renderSubmesh.material.alphaCutoff = ClampMaterialAlphaValue(material.alphaCutoff, 0.5f);
        renderSubmesh.material.surfaceFactors[0] = material.metallicFactor;
        renderSubmesh.material.surfaceFactors[1] = material.roughnessFactor;
        renderSubmesh.material.surfaceFactors[2] = material.normalScale;
        renderSubmesh.material.surfaceFactors[3] = material.occlusionStrength;
        renderSubmesh.material.nodeGraphFactors[0] = material.blendGraph.enabled ? 1.0f : 0.0f;
        renderSubmesh.material.nodeGraphFactors[1] = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);
        renderSubmesh.material.nodeGraphFactors[2] = 1.0f;
        renderSubmesh.material.nodeGraphFactors[3] = 0.0f;
        // A coat of zero is no coat, a black sheen no sheen: those draws keep the plain base and its
        // exact shading.
        const float clearcoat = std::clamp(material.clearcoatFactor, 0.0f, 1.0f);
        float sheenStrength = 0.0f;
        for (size_t index = 0; index < 3; ++index)
        {
            renderSubmesh.material.sheenFactors[index] = std::clamp(material.sheenColorFactor[index], 0.0f, 1.0f);
            sheenStrength = std::max(sheenStrength, renderSubmesh.material.sheenFactors[index]);
        }
        renderSubmesh.material.sheenFactors[3] = std::clamp(material.sheenRoughnessFactor, 0.0f, 1.0f);
        const float anisotropyStrength = std::clamp(material.anisotropyStrength, 0.0f, 1.0f);
        const bool anisotropic = anisotropyStrength > 0.0f;
        // The dielectric's reflectance, stored before the maps: the IOR's F0 tinted by the colour
        // factor, and the specular factor. A surface at the defaults, with no maps, keeps the plain
        // path (F0 0.04, F90 1) without reading GB5.
        const float ior = SanitizeIor(material.ior);
        const float reflectance = ior == 0.0f ? 1.0f : ((ior - 1.0f) / (ior + 1.0f)) * ((ior - 1.0f) / (ior + 1.0f));
        for (size_t index = 0; index < 3; ++index)
        {
            renderSubmesh.material.specularFactors[index] = reflectance * std::max(material.specularColorFactor[index], 0.0f);
        }
        renderSubmesh.material.specularFactors[3] = std::clamp(material.specularFactor, 0.0f, 1.0f);
        const bool customSpecular =
            ior != 1.5f || material.specularFactor != 1.0f || material.specularColorFactor[0] != 1.0f ||
            material.specularColorFactor[1] != 1.0f || material.specularColorFactor[2] != 1.0f ||
            (submesh.hasTexCoords && (!material.specularTexturePath.empty() || !material.specularColorTexturePath.empty()));
        const bool coatNormal = clearcoat > 0.0f && submesh.hasTexCoords && !material.clearcoatNormalTexturePath.empty();
        // A thin film has no room in the G-buffer: it sends the material to the forward pass.
        const float iridescence = std::clamp(material.iridescenceFactor, 0.0f, 1.0f);
        renderSubmesh.material.iridescenceFactors[0] = iridescence;
        renderSubmesh.material.iridescenceFactors[1] = std::max(material.iridescenceIor, 1.0f);
        renderSubmesh.material.iridescenceFactors[2] = std::max(material.iridescenceThicknessMinimum, 0.0f);
        renderSubmesh.material.iridescenceFactors[3] = std::max(material.iridescenceThicknessMaximum, 0.0f);
        renderSubmesh.material.shadingModel[0] =
            (clearcoat > 0.0f ? kShadingFlagClearcoat : 0u) | (sheenStrength > 0.0f ? kShadingFlagSheen : 0u) |
            (anisotropic ? kShadingFlagAnisotropy : 0u) | (customSpecular ? kShadingFlagSpecular : 0u) |
            (coatNormal ? kShadingFlagCoatNormal : 0u) | (iridescence > 0.0f ? kShadingFlagForward : 0u);
        // Transmission sends the material to the forward pass, drawn over a copy of the scene behind
        // it; the volume's thickness is in mesh units, its attenuation distance in metres.
        const float transmission = std::clamp(material.transmissionFactor, 0.0f, 1.0f);
        renderSubmesh.material.transmissionFactors[0] = transmission;
        renderSubmesh.material.transmissionFactors[1] = std::max(material.thicknessFactor, 0.0f);
        renderSubmesh.material.transmissionFactors[2] = std::max(material.attenuationDistance, 0.0f);
        for (size_t index = 0; index < 3; ++index)
        {
            renderSubmesh.material.attenuationColor[index] = std::clamp(material.attenuationColor[index], 0.0f, 1.0f);
        }
        // The refraction IOR; KHR_materials_ior's 0 (an infinite index) bends every ray to the normal.
        renderSubmesh.material.attenuationColor[3] = ior == 0.0f ? 1000.0f : ior;
        renderSubmesh.material.volumeScale[0] = submesh.nodeScale.x;
        renderSubmesh.material.volumeScale[1] = submesh.nodeScale.y;
        renderSubmesh.material.volumeScale[2] = submesh.nodeScale.z;
        if (transmission > 0.0f)
        {
            renderSubmesh.material.shadingModel[0] |= kShadingFlagTransmission | kShadingFlagForward;
        }
        renderSubmesh.material.transmissionFactors[3] = std::max(material.dispersion, 0.0f);
        // Diffuse transmission is shaded by the forward pass, which reads the factor itself.
        const float diffuseTransmission = std::clamp(material.diffuseTransmissionFactor, 0.0f, 1.0f);
        for (size_t index = 0; index < 3; ++index)
        {
            renderSubmesh.material.diffuseTransmission[index] = std::clamp(material.diffuseTransmissionColor[index], 0.0f, 1.0f);
        }
        renderSubmesh.material.diffuseTransmission[3] = diffuseTransmission;
        if (diffuseTransmission > 0.0f)
        {
            renderSubmesh.material.shadingModel[0] |= kShadingFlagForward;
        }
        // Volume scatter diffuses the light that diffuse transmission lets into the volume; without it
        // the Khronos sample viewer's pre-pass gathers nothing, so nothing scatters.
        if (material.volumeScatter && diffuseTransmission > 0.0f)
        {
            renderSubmesh.material.volumeScale[3] = 1.0f;
            for (size_t index = 0; index < 3; ++index)
            {
                renderSubmesh.material.volumeScatter[index] = std::clamp(material.multiscatterColor[index], 0.0f, 1.0f);
            }
            renderSubmesh.material.volumeScatter[3] = ClampScatterAnisotropy(material.scatterAnisotropy);
        }
        renderSubmesh.material.clearcoatFactors[2] = material.clearcoatNormalScale;
        // Unlit shows the base colour alone; transforms only matter where there are textures.
        if (material.unlit)
        {
            renderSubmesh.material.shadingModel[0] |= kShadingFlagUnlit;
        }
        renderSubmesh.textureSamplers = material.textureSamplers;
        if (submesh.hasTexCoords && !AreIdentity(material.textureTransforms))
        {
            renderSubmesh.textureTransforms = material.textureTransforms;
            renderSubmesh.material.shadingModel[1] = 1u;
        }
        renderSubmesh.material.anisotropyFactors[0] = anisotropic ? anisotropyStrength : 0.0f;
        renderSubmesh.material.anisotropyFactors[1] = std::cos(material.anisotropyRotation);
        renderSubmesh.material.anisotropyFactors[2] = std::sin(material.anisotropyRotation);
        renderSubmesh.material.clearcoatFactors[0] = clearcoat;
        renderSubmesh.material.clearcoatFactors[1] = std::clamp(material.clearcoatRoughnessFactor, 0.0f, 1.0f);
        renderSubmesh.name = submesh.name;
        if (submesh.hasTexCoords)
        {
            renderSubmesh.textures.baseColor = model.baseColorTextureOverridePath.empty()
                                                   ? resolveTex(material.baseColorTexturePath)
                                                   : model.baseColorTextureOverridePath;
            renderSubmesh.textures.normal = resolveTex(material.normalTexturePath);
            renderSubmesh.textures.metallic = resolveTex(material.metallicTexturePath);
            renderSubmesh.textures.roughness = resolveTex(material.roughnessTexturePath);
            renderSubmesh.textures.occlusion = resolveTex(material.occlusionTexturePath);
            renderSubmesh.textures.emissive = resolveTex(material.emissiveTexturePath);
            renderSubmesh.textures.secondaryBaseColor = material.blendGraph.secondaryBaseColorTexturePath.empty()
                                                            ? renderSubmesh.textures.baseColor
                                                            : material.blendGraph.secondaryBaseColorTexturePath;
            renderSubmesh.textures.secondaryNormal = material.blendGraph.secondaryNormalTexturePath.empty()
                                                         ? renderSubmesh.textures.normal
                                                         : material.blendGraph.secondaryNormalTexturePath;
            renderSubmesh.textures.secondaryMetallic = material.blendGraph.secondaryMetallicTexturePath.empty()
                                                           ? renderSubmesh.textures.metallic
                                                           : material.blendGraph.secondaryMetallicTexturePath;
            renderSubmesh.textures.secondaryRoughness = material.blendGraph.secondaryRoughnessTexturePath.empty()
                                                            ? renderSubmesh.textures.roughness
                                                            : material.blendGraph.secondaryRoughnessTexturePath;
            renderSubmesh.textures.secondaryOcclusion = material.blendGraph.secondaryOcclusionTexturePath.empty()
                                                            ? renderSubmesh.textures.occlusion
                                                            : material.blendGraph.secondaryOcclusionTexturePath;
            renderSubmesh.textures.secondaryEmissive = material.blendGraph.secondaryEmissiveTexturePath.empty()
                                                           ? renderSubmesh.textures.emissive
                                                           : material.blendGraph.secondaryEmissiveTexturePath;
            renderSubmesh.textures.blendMask = material.blendGraph.blendMaskTexturePath;
            renderSubmesh.textures.clearcoat = resolveTex(material.clearcoatTexturePath);
            renderSubmesh.textures.clearcoatRoughness = resolveTex(material.clearcoatRoughnessTexturePath);
            renderSubmesh.textures.sheenColor = resolveTex(material.sheenColorTexturePath);
            renderSubmesh.textures.sheenRoughness = resolveTex(material.sheenRoughnessTexturePath);
            renderSubmesh.textures.anisotropy = resolveTex(material.anisotropyTexturePath);
            renderSubmesh.textures.specular = resolveTex(material.specularTexturePath);
            renderSubmesh.textures.specularColor = resolveTex(material.specularColorTexturePath);
            renderSubmesh.textures.clearcoatNormal = resolveTex(material.clearcoatNormalTexturePath);
            renderSubmesh.textures.iridescence = resolveTex(material.iridescenceTexturePath);
            renderSubmesh.textures.iridescenceThickness = resolveTex(material.iridescenceThicknessTexturePath);
            renderSubmesh.textures.transmission = resolveTex(material.transmissionTexturePath);
            renderSubmesh.textures.thickness = resolveTex(material.thicknessTexturePath);
            renderSubmesh.textures.diffuseTransmission = resolveTex(material.diffuseTransmissionTexturePath);
            renderSubmesh.textures.diffuseTransmissionColor = resolveTex(material.diffuseTransmissionColorTexturePath);
        }
        renderSubmeshes.push_back(std::move(renderSubmesh));
    }

    world.UpdateModelInfo(
        entity,
        model.displayName,
        model.sourcePath,
        static_cast<uint32_t>(modelData.submeshes.size()),
        modelData.minBounds,
        modelData.maxBounds,
        modelData.hasBounds,
        importedMaterials,
        importedSubmeshes,
        modelData.materialVariants,
        static_cast<uint32_t>(modelData.lights.size()));
    return renderSubmeshes;
}

// The scene's live set only changes when renderables are rebuilt or refreshed,
// so eviction is evaluated there rather than per frame, where it would almost
// always be a no-op.
void TrimModelCache(RendererSharedState& state)
{
    const entt::registry& registry = state.GetEditorWorld().Registry();
    std::unordered_set<std::string> liveKeys;
    for (entt::entity entity : registry.view<const ModelComponent>())
    {
        const ModelComponent& model = registry.get<ModelComponent>(entity);
        if (!model.sourcePath.empty())
        {
            liveKeys.insert(model.sourcePath);
        }
    }
    ModelCache::Trim(liveKeys, kDefaultModelCacheBudgetBytes);
}
}

// The lights a model entity's model carries, while its model lights are on; none for the default
// cube or a model not loaded yet.
std::vector<CpuModelLight> BuildEntityModelLights(RendererSharedState& state, entt::entity entity)
{
    const ModelComponent& model = state.GetEditorWorld().GetModel(entity);
    std::vector<CpuModelLight> lights;
    if (model.sourcePath.empty() || !model.useModelLights)
    {
        return lights;
    }
    const std::shared_ptr<const LoadedModelData> modelData = ModelCache::Get(model.sourcePath);
    if (!modelData)
    {
        return lights;
    }
    lights.reserve(modelData->lights.size());
    for (const ModelLightData& source : modelData->lights)
    {
        CpuModelLight light{};
        light.entity = entity;
        light.light.type = source.type;
        light.light.color = source.color;
        light.light.intensity = source.intensity;
        light.light.range = source.range;
        light.light.spotInnerAngleDegrees = source.innerAngleDegrees;
        light.light.spotOuterAngleDegrees = source.outerAngleDegrees;
        light.position = source.position;
        light.direction = source.direction;
        lights.push_back(light);
    }
    return lights;
}

void RebuildSceneRenderables(RendererSharedState& state)
{
    std::vector<CpuRenderSubmesh> newRenderSubmeshes;
    std::vector<CpuModelLight> newModelLights;
    IEditorWorld& world = state.GetEditorWorld();
    for (entt::entity entity : world.Registry().view<const ModelComponent>())
    {
        std::vector<CpuRenderSubmesh> entitySubmeshes = BuildEntityRenderSubmeshes(state, entity);
        newRenderSubmeshes.insert(
            newRenderSubmeshes.end(),
            std::make_move_iterator(entitySubmeshes.begin()),
            std::make_move_iterator(entitySubmeshes.end()));
        std::vector<CpuModelLight> entityLights = BuildEntityModelLights(state, entity);
        newModelLights.insert(newModelLights.end(), entityLights.begin(), entityLights.end());
    }

    state.rendererWorld.SetRenderSubmeshes(std::move(newRenderSubmeshes));
    state.rendererWorld.SetModelLights(std::move(newModelLights));
    world.ClearAllModelRenderableDirty();
    state.renderablesDirty = true;
    TrimModelCache(state);
}

bool RefreshDirtySceneRenderables(RendererSharedState& state)
{
    IEditorWorld& world = state.GetEditorWorld();
    const std::vector<entt::entity> dirtyEntities = world.GetDirtyModelRenderableEntities();

    std::vector<std::pair<entt::entity, std::vector<CpuRenderSubmesh>>> replacements;
    replacements.reserve(dirtyEntities.size());
    for (entt::entity entity : dirtyEntities)
    {
        replacements.emplace_back(entity, BuildEntityRenderSubmeshes(state, entity));
    }

    bool changed = false;
    std::unordered_set<entt::entity> staleEntities;
    for (const CpuRenderSubmesh& submesh : state.rendererWorld.GetRenderSubmeshes())
    {
        if (!world.HasModelComponent(submesh.entity))
        {
            staleEntities.insert(submesh.entity);
        }
    }
    for (entt::entity entity : staleEntities)
    {
        changed |= state.rendererWorld.RemoveEntityRenderSubmeshes(entity);
        changed |= state.rendererWorld.RemoveEntityModelLights(entity);
    }

    for (auto& [entity, renderSubmeshes] : replacements)
    {
        state.rendererWorld.ReplaceEntityRenderSubmeshes(entity, std::move(renderSubmeshes));
        state.rendererWorld.ReplaceEntityModelLights(entity, BuildEntityModelLights(state, entity));
        world.ClearModelRenderableDirty(entity);
        changed = true;
    }

    state.renderablesDirty |= changed;
    if (changed)
    {
        TrimModelCache(state);
    }
    return changed;
}

void MarkModelRenderablesDirtyForSourcePath(RendererSharedState& state, const std::string& sourcePath)
{
    const std::filesystem::path targetPath = std::filesystem::path(sourcePath).lexically_normal();
    IEditorWorld& world = state.GetEditorWorld();
    const entt::registry& registry = world.Registry();
    for (entt::entity entity : registry.view<const ModelComponent>())
    {
        const ModelComponent& model = registry.get<ModelComponent>(entity);
        if (std::filesystem::path(model.sourcePath).lexically_normal() == targetPath)
        {
            world.MarkModelRenderableDirty(entity);
        }
    }
}
}
