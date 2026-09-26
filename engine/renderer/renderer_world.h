#pragma once

#include "material.h"
#include <engine/asset/mesh.h>

#include <engine/scene/scene_components.h>
#include <engine/scene/scene_world.h>

#include <memory>
#include <stdexcept>
#include <string>
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
};

struct CpuRenderSubmesh
{
    entt::entity entity = entt::null;
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
    const std::vector<CpuRenderSubmesh>& GetRenderSubmeshes() const;
    glm::mat4 GetModelMatrix(entt::entity entity) const;

    void SetModelLights(std::vector<CpuModelLight> modelLights);
    void ReplaceEntityModelLights(entt::entity entity, std::vector<CpuModelLight> modelLights);
    bool RemoveEntityModelLights(entt::entity entity);
    const std::vector<CpuModelLight>& GetModelLights() const;

  private:
    ISceneWorld* m_sceneWorld = nullptr;
    std::vector<CpuRenderSubmesh> m_renderSubmeshes;
    std::vector<CpuModelLight> m_modelLights;
};
}
