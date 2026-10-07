#pragma once

#include "material_graph.h"
#include "world_units.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace me
{

enum class LightType : uint32_t
{
    Directional = 0, // Global sun-like light, uses transform rotation for direction
    Point = 1,       // Omnidirectional point light
    Spot = 2,        // Cone spotlight
    Area = 3,        // Rectangular area light (approximate)
    Ambient = 4,     // Global ambient (no position or direction)
    Hemisphere = 5   // Sky colour above the transform's up axis, ground colour below (no position)
};

struct LightComponent
{
    LightType type = LightType::Point;
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1000.0f;                // Lumens (point/spot/area), lux (directional) or cd/m^2 (ambient, hemisphere)
    float range = 10.0f;                      // Effective range in meters
    float spotInnerAngleDegrees = 15.0f;      // Spot inner cone half-angle
    float spotOuterAngleDegrees = 30.0f;      // Spot outer cone half-angle
    glm::vec2 areaSize{1.0f, 1.0f};           // Area light width x height in meters
    bool castShadows = true;                  // Point, spot and area lights; the brightest directional one always does
    float sourceRadius = 0.0f;                // Point and spot lights: the emitting sphere's radius in metres, 0 a point
    glm::vec3 groundColor{0.3f, 0.25f, 0.2f}; // Hemisphere lights: the colour below the horizon; color is the sky's
};

struct ModelImportedMaterialInfo
{
    std::string name;
    std::string baseColorTexturePath;
    std::string normalTexturePath;
    std::string metallicTexturePath;
    std::string roughnessTexturePath;
    std::string occlusionTexturePath;
    std::string emissiveTexturePath;
    std::string clearcoatTexturePath;
    std::string clearcoatRoughnessTexturePath;
    std::string sheenColorTexturePath;
    std::string sheenRoughnessTexturePath;
    std::string anisotropyTexturePath;
    std::string specularTexturePath;
    std::string specularColorTexturePath;
    std::string clearcoatNormalTexturePath;
    std::string iridescenceTexturePath;
    std::string iridescenceThicknessTexturePath;
    std::string transmissionTexturePath;
    std::string thicknessTexturePath;
    std::string diffuseTransmissionTexturePath;
    std::string diffuseTransmissionColorTexturePath;
    MaterialPbrSurfaceSettings pbr;
    MaterialTextureBlendGraph blendGraph;
    MaterialShaderGraph shaderGraph;
    MaterialTextureTransforms textureTransforms{};
    MaterialTextureSamplers textureSamplers{};
    MaterialDetailLayers detailLayers;
};

struct ModelImportedSubmeshInfo
{
    std::string name;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    bool hasTexCoords = false;
    bool hasNormals = false;
    bool hasTangents = false;
};

struct SceneEntityIdComponent
{
    std::string value;
};

// A model entity that world streaming made (WorldStreamingService): it exists while its cell is
// wanted, and is never saved, listed in the scene panel or selected.
struct StreamedComponent
{
    std::string cell;
    bool lod = false;
};

struct TagComponent
{
    std::string name = "Cube";
};

struct TransformComponent
{
    glm::vec3 translation{0.0f, 0.0f, 0.0f};
    glm::vec3 rotationDegrees{0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f, 1.0f, 1.0f};
};

struct WorldTransformComponent
{
    glm::mat4 matrix{1.0f};
};

// Empty tags consumed in batches by scene/render systems.
struct TransformDirty
{
};
struct ModelRenderableDirty
{
};

// How a driver's hands hold the steering wheel, set by hand (in the Inspector, or with the transform
// gizmo on a wrist) where the fitted grip is not right for a character (VehicleDriverService). The left
// hand's; the right's is its mirror image.
struct DriverGripCalibration
{
    // Where the hands hold the rim at rest: degrees from the top (90, a quarter to three).
    float holdAtDegrees = 90.0f;
    // The wrist moved from where the grip is fitted (metres) and turned about itself (degrees, as glm's
    // Euler angles), both in the grip's frame there: X out from the wheel's centre, Y along the rim
    // (clockwise as the driver sees it, for the left hand), Z along the column away from the driver.
    glm::vec3 wristOffset{0.0f};
    glm::vec3 wristTurnDegrees{0.0f};
    // Added to every finger joint's bend (degrees; negative opens the hand).
    float fingerCurlDegrees = 0.0f;
    // The thumb laid on the rim; off, it stays out.
    bool thumbOnRim = true;

    bool operator==(const DriverGripCalibration&) const = default;
};

struct ModelComponent
{
    std::string sourcePath;
    std::string displayName = "Cube";
    std::string baseColorTextureOverridePath;
    // Stable asset ids matching sourcePath / baseColorTextureOverridePath;
    // they let serialized references survive asset renames and moves.
    std::string sourceUuid;
    std::string baseColorTextureOverrideUuid;
    // KHR_materials_variants: the name of the variant this entity wears, empty for the glTF's
    // default bindings. By name so a re-exported glTF that reorders its variants keeps the choice.
    std::string materialVariant;
    // KHR_lights_punctual: whether the lights the model carries shine, through this entity's transform.
    bool useModelLights = true;
    // glTF animations (ModelSkeleton::clips), for a model that has them: the clip by name, empty for
    // the automatic choice (one named "idle", else the first); off, the skinned meshes stay in their
    // bind pose. Paused, the clip holds its frame; speed scales its time.
    std::string animationClip;
    bool animationEnabled = true;
    bool animationPlaying = true;
    float animationSpeed = 1.0f;
    // Hair and skirts swing (SimulateSpringBones); off, they keep the animated pose.
    bool springBones = true;
    // Seats this model in a car as its driver: the car's entity (SceneEntityIdComponent), empty for
    // none. The model then follows the car, and a humanoid one sits with its hands on the steering
    // wheel (VehicleDriverService) instead of playing its clip.
    std::string driverVehicleUuid;
    // Moves the seat from where it is fitted (metres: to the car's right, up, forward).
    glm::vec3 driverSeatOffset{0.0f};
    DriverGripCalibration driverGrip;
};

struct ModelBoundsComponent
{
    glm::vec3 minBounds = WorldUnits::kDefaultCubeMinBoundsMeters;
    glm::vec3 maxBounds = WorldUnits::kDefaultCubeMaxBoundsMeters;
    bool hasBounds = true;
};

// Editor-only import details are kept out of the runtime-facing model and
// bounds components so hot ECS views do not pull this cold, variable data.
struct EditorModelMetadataComponent
{
    uint32_t submeshCount = 1;
    std::vector<ModelImportedMaterialInfo> importedMaterials;
    std::vector<ModelImportedSubmeshInfo> importedSubmeshes;
    // KHR_materials_variants' names, for the variant picker.
    std::vector<std::string> materialVariants;
    // How many KHR_lights_punctual lights the model carries (the model lights checkbox shows then).
    uint32_t modelLightCount = 0;
};
}
