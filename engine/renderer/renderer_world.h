#pragma once

#include "material.h"
#include <engine/asset/mesh.h>

#include <engine/scene/scene_components.h>
#include <engine/scene/scene_world.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace me
{

struct MaterialTexturePaths
{
    std::string baseColor;
    std::string normal;
    std::string metallic;
    std::string roughness;
    std::string occlusion;
    std::string emissive;
    std::string secondaryBaseColor;
    std::string secondaryNormal;
    std::string secondaryMetallic;
    std::string secondaryRoughness;
    std::string secondaryOcclusion;
    std::string secondaryEmissive;
    std::string blendMask;
    // The layer maps; only the primary layer has them.
    std::string clearcoat;
    std::string clearcoatRoughness;
    std::string sheenColor;
    std::string sheenRoughness;
    std::string anisotropy;
    std::string specular;
    std::string specularColor;
    std::string clearcoatNormal;
    std::string iridescence;
    std::string iridescenceThickness;
    std::string transmission;
    std::string thickness;
    std::string diffuseTransmission;
    std::string diffuseTransmissionColor;
    // MaterialDetailLayers: the mask, then the layers its channels R, G, B and A weigh.
    std::string detailMask;
    std::array<std::string, kDetailLayerCount> detailLayers{};
};

struct CpuRenderSubmesh
{
    entt::entity entity = entt::null;
    // Unique to this submesh as set: SetRenderSubmeshes and ReplaceEntityRenderSubmeshes number every
    // submesh they are given, and nothing changes one afterwards, so a render backend that saw this
    // revision before can keep what it made of it.
    uint64_t revision = 0;
    // Shared with the model cache rather than copied out of it: a scene like Sponza has
    // hundreds of submeshes, nothing here mutates the geometry, and the entry this aliases
    // keeps the whole cached model alive for as long as any submesh references it.
    std::shared_ptr<const MeshData> mesh;
    GpuMaterialData material;
    MaterialTexturePaths textures;
    MaterialTextureTransforms textureTransforms{};
    // Each slot's glTF sampler: wrapping and filtering (VulkanSamplerCache).
    MaterialTextureSamplers textureSamplers{};
    bool hasTexCoords = false;
    bool doubleSided = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    // A Blend material drawn as a deferred decal (MaterialPbrSurfaceSettings::decal); never a forward-
    // shaded, unlit or transmissive one.
    bool decal = false;
    // The top of water (ModelSubmeshData::water): drawn, and never collided with.
    bool water = false;
    glm::vec3 localBoundsCenter{0.0f};
    float localBoundsRadius = 0.0f;
    std::string name;
};

// A light a model carries (KHR_lights_punctual), in the model's space: it shines through its
// entity's transform while ModelComponent::useModelLights is on. Not a scene entity, so saving and
// reloading a scene never duplicates it.
struct CpuModelLight
{
    entt::entity entity = entt::null;
    // Type, colour, intensity (engine units), range and cone; the transform fields go unused.
    LightComponent light;
    glm::vec3 position{0.0f};
    // Where the light travels, unit length.
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

// The world's render submeshes. Each is immutable once the world numbered it, so a copy of the list
// shares the submeshes rather than copying them.
using CpuRenderSubmeshList = std::vector<std::shared_ptr<const CpuRenderSubmesh>>;

class RendererWorld
{
  public:
    void SetSceneWorld(ISceneWorld& sceneWorld);
    bool HasSceneWorld() const;
    ISceneWorld& GetSceneWorld();
    const ISceneWorld& GetSceneWorld() const;

    void SetRenderSubmeshes(std::vector<CpuRenderSubmesh> renderSubmeshes);
    void ReplaceEntityRenderSubmeshes(entt::entity entity, std::vector<CpuRenderSubmesh> renderSubmeshes);
    bool RemoveEntityRenderSubmeshes(entt::entity entity);
    void ClearRenderSubmeshes();
    const CpuRenderSubmeshList& GetRenderSubmeshes() const;
    // The list as it is now, for the render thread to draw from while this world changes: the same
    // snapshot until the list changes, so an unchanged scene shares one.
    std::shared_ptr<const CpuRenderSubmeshList> SnapshotRenderSubmeshes() const;
    glm::mat4 GetModelMatrix(entt::entity entity) const;

    // A submesh's own transform inside its model, applied before the entity's: a car's wheels turn
    // and ride the suspension while the rest of the model stays put. `ordinal` is the submesh's
    // position among the entity's submeshes; one that has no transform, or none set, is drawn at the
    // entity's matrix.
    void SetSubmeshLocalTransforms(entt::entity entity, std::vector<glm::mat4> transforms);
    void ClearSubmeshLocalTransforms(entt::entity entity);
    glm::mat4 GetSubmeshModelMatrix(entt::entity entity, uint32_t ordinal) const;
    const std::unordered_map<entt::entity, std::vector<glm::mat4>>& GetSubmeshLocalTransforms() const;

    void SetModelLights(std::vector<CpuModelLight> modelLights);
    void ReplaceEntityModelLights(entt::entity entity, std::vector<CpuModelLight> modelLights);
    bool RemoveEntityModelLights(entt::entity entity);
    const std::vector<CpuModelLight>& GetModelLights() const;

  private:
    ISceneWorld* m_sceneWorld = nullptr;
    // Numbers the submeshes and makes them shared and immutable.
    std::vector<std::shared_ptr<const CpuRenderSubmesh>> Number(std::vector<CpuRenderSubmesh> renderSubmeshes);

    CpuRenderSubmeshList m_renderSubmeshes;
    // SnapshotRenderSubmeshes' copy; dropped whenever the list changes.
    mutable std::shared_ptr<const CpuRenderSubmeshList> m_snapshot;
    uint64_t m_nextRevision = 1;
    std::vector<CpuModelLight> m_modelLights;
    std::unordered_map<entt::entity, std::vector<glm::mat4>> m_submeshLocalTransforms;
};
}
