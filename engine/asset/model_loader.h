#pragma once

#include "kn5_importer.h"
#include "mesh.h"

#include <engine/scene/material_graph.h>
#include <engine/scene/scene_components.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace me
{

struct ModelMaterialData
{
    std::string name;
    std::string baseColorTexturePath;
    std::string normalTexturePath;
    std::string metallicTexturePath;
    std::string roughnessTexturePath;
    std::string occlusionTexturePath;
    std::string emissiveTexturePath;
    // The layer maps (KHR_materials_clearcoat, _sheen, _anisotropy). Each multiplies its factor.
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
    // KHR_materials_transmission (R) and KHR_materials_volume (G); each multiplies its factor.
    std::string transmissionTexturePath;
    std::string thicknessTexturePath;
    // KHR_materials_diffuse_transmission: the factor's map (A) and the colour's (RGB, sRGB).
    std::string diffuseTransmissionTexturePath;
    std::string diffuseTransmissionColorTexturePath;
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    float opacity = 1.0f;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    float sheenColorFactor[3] = {0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    float ior = 1.5f;
    float specularFactor = 1.0f;
    float specularColorFactor[3] = {1.0f, 1.0f, 1.0f};
    float clearcoatNormalScale = 1.0f;
    float iridescenceFactor = 0.0f;
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMinimum = 100.0f;
    float iridescenceThicknessMaximum = 400.0f;
    float transmissionFactor = 0.0f;
    float thicknessFactor = 0.0f;
    float attenuationDistance = 0.0f;
    float attenuationColor[3] = {1.0f, 1.0f, 1.0f};
    float dispersion = 0.0f;
    float diffuseTransmissionFactor = 0.0f;
    float diffuseTransmissionColor[3] = {1.0f, 1.0f, 1.0f};
    bool volumeScatter = false;
    float multiscatterColor[3] = {0.0f, 0.0f, 0.0f};
    float scatterAnisotropy = 0.0f;
    MaterialPbrSurfaceSettings pbr;
    MaterialTextureBlendGraph blendGraph;
    MaterialShaderGraph shaderGraph;
    MaterialDetailLayers detailLayers;
    MaterialTextureTransforms textureTransforms{};
    // Each slot's glTF sampler; the metallic and roughness slots share the metallic-roughness one.
    MaterialTextureSamplers textureSamplers{};
    bool unlit = false;
    // MaterialPbrSurfaceSettings::decal.
    bool decal = false;
};

// What a car model's node says a submesh is, by Assetto Corsa's naming (kn5 imports keep the
// names): under WHEEL_xx the tyre and rim, under DISC_xx the brake disc, under SUSP_xx the hub, arms
// and dampers. xx is LF, RF, LR or RR.
enum class ModelWheelPart : uint8_t
{
    None = 0,
    Wheel, // turns with the steering and the axle, rides the suspension
    Disc,  // turns with the steering, rides the suspension
    Suspension // rides the suspension
};

// The order of the corners: left front, right front, left rear, right rear.
inline constexpr size_t kModelWheelCornerCount = 4;

struct ModelSubmeshData
{
    MeshData mesh;
    // Computed once on the loading thread (ModelPostProcess::FinalizeSubmeshData) so that
    // building renderables doesn't have to walk every vertex again on the main thread.
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
    // The same box as the Khronos glTF Sample Viewer measures it, for framing like it: the POSITION
    // accessor's min/max box transformed by the node, its axis-aligned bounds' centre and
    // half-diagonal. Larger than the vertices' own box under a rotated node. The vertices' bounds
    // when the accessor has no min/max.
    glm::vec3 viewerBoundsCenter{0.0f};
    float viewerBoundsRadius = 0.0f;
    // The glTF node's world scale per axis (its matrix's column lengths). The node transform is baked
    // into the vertices, but a volume's thickness (KHR_materials_volume) is in mesh units and scales
    // with it.
    glm::vec3 nodeScale{1.0f};
    uint32_t materialIndex = 0;
    ModelWheelPart wheelPart = ModelWheelPart::None;
    // Which corner's node the submesh hangs under, 0 to 3, when wheelPart is not None.
    uint8_t wheelCorner = 0;
    // Under the STEER_HR node: the steering wheel, which turns about LoadedModelData::steeringWheel.
    bool steeringWheel = false;
    // KHR_materials_variants: the material each of the model's variants gives this primitive, one
    // entry per LoadedModelData::materialVariants (the primitive's own material where the variant
    // has no mapping). Empty for a model without variants.
    std::vector<uint32_t> variantMaterialIndices;
    bool hasTexCoords = false;
    bool hasNormals = false;
    bool hasTangents = false;
    std::string name;
};

// A light the model carries (KHR_lights_punctual), in the model's space and the engine's units:
// lumens for point and spot lights (glTF's candela times the light's solid angle), lux for
// directional ones. It shines through the model entity's transform, not as a scene entity.
struct ModelLightData
{
    std::string name;
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};
    float intensity = 0.0f;
    // Metres; glTF's undefined (infinite) range made finite where the light falls to 1e-3 lux.
    float range = 10.0f;
    // Spot cone half-angles.
    float innerAngleDegrees = 0.0f;
    float outerAngleDegrees = 45.0f;
    glm::vec3 position{0.0f};
    // Where the light travels: the node's -Z, unit length.
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

// The steering wheel as the STEER_HR node defines it, in the model's space: it turns about its
// column, the node's local Z (pointing forward and down in Assetto Corsa cars).
struct ModelSteeringWheel
{
    glm::vec3 center{0.0f};
    glm::vec3 axis{0.0f, 0.0f, 1.0f};
};

// A car's four wheels as its WHEEL_LF, WHEEL_RF, WHEEL_LR and WHEEL_RR nodes define them, in the
// model's space (the node transforms are baked into the vertices, so these match them).
struct ModelWheelRig
{
    struct Corner
    {
        // The wheel node's origin: the wheel's centre at rest.
        glm::vec3 center{0.0f};
        // The outermost reach of the node's meshes about the axle, and their extent along it.
        float radius = 0.0f;
        float width = 0.0f;
    };
    std::array<Corner, kModelWheelCornerCount> corners;
    // From the right wheels' centres to the left's, unit length: the axle the wheels spin about.
    glm::vec3 axle{1.0f, 0.0f, 0.0f};
};

// Geometry that collides and is never drawn: Assetto Corsa's physics meshes, which the kn5 import
// writes as glTF nodes carrying MINIENGINE_collision. The meshes of one surface are merged, in
// the model's space (node transforms baked in, as the submeshes' are).
struct ModelCollisionMesh
{
    // The surface's name as its source calls it ("ASPH-SPA_BLACK", "GRASS", "WALL").
    std::string surface;
    // The surface's friction coefficient, about 1 for tarmac and 0.6 for grass.
    float friction = 1.0f;
    std::vector<glm::vec3> positions;
    // Three indices into `positions` per triangle.
    std::vector<uint32_t> indices;
};

struct LoadedModelData
{
    std::vector<ModelMaterialData> materials;
    std::vector<ModelSubmeshData> submeshes;
    // When present, a physics simulation collides with these instead of the submeshes.
    std::vector<ModelCollisionMesh> collisionMeshes;
    std::vector<ModelLightData> lights;
    // KHR_materials_variants' names, in the glTF's order.
    std::vector<std::string> materialVariants;
    // Set when all four WHEEL_xx nodes are there and have meshes.
    std::optional<ModelWheelRig> wheelRig;
    // Set when a STEER_HR node has meshes.
    std::optional<ModelSteeringWheel> steeringWheel;
    glm::vec3 minBounds{0.0f, 0.0f, 0.0f};
    glm::vec3 maxBounds{0.0f, 0.0f, 0.0f};
    bool hasBounds = false;

    bool IsValid() const
    {
        return !submeshes.empty();
    }
};

// The material a submesh draws with under a variant (an index into materialVariants); its own
// material for no variant or an index past its list.
uint32_t ResolveSubmeshMaterialIndex(const ModelSubmeshData& submesh, std::optional<uint32_t> variantIndex);

// The index of the variant with this name; nullopt for the empty name (the default bindings) and
// for a name the model does not have.
std::optional<uint32_t> FindMaterialVariant(const LoadedModelData& model, const std::string& name);

// The Khronos glTF Sample Viewer's scene extents (getSceneExtents), for framing like it: each
// submesh's viewer box (viewerBoundsCenter/Radius) grown to the cube around its bounding sphere, placed by
// modelMatrix, and their union. False, leaving the outputs untouched, for a model without submeshes.
bool ComputeKhronosViewerExtents(const LoadedModelData& model, const glm::mat4& modelMatrix, glm::vec3& minBounds, glm::vec3& maxBounds);

// Invoked from the loading thread with the overall load fraction in [0, 1].
// Implementations must be cheap and thread-safe (typically an atomic store).
using ModelLoadProgressCallback = std::function<void(float)>;

class ModelLoader
{
  public:
    // Formats LoadModel reads: glTF 2.0 (.gltf, .glb).
    static bool IsSupportedModelPath(const std::filesystem::path& path);
    // Formats an import accepts: the loadable ones, plus Assetto Corsa .kn5 and track layouts
    // (models*.ini), which an import converts into a glTF bundle (see Kn5Importer).
    static bool IsImportableModelPath(const std::filesystem::path& path);
    // The name an import of `path` gives its folder and model: the file's stem, or a track
    // layout's name (Kn5Importer::ImportName).
    static std::string ImportName(const std::filesystem::path& path);
    static const char* GetImporterName();
    static LoadedModelData LoadModel(const std::string& path, const ModelLoadProgressCallback& progress = {});

    // Copies a model into targetDirectory. For an ASCII .gltf the referenced
    // companion files are copied too, sorted into subfolders (buffers/,
    // textures/) with the glTF's URIs rewritten to match; .glb is copied
    // as-is; a .kn5 or a track layout is converted into "<ImportName>.gltf"
    // with its buffer and PNG textures laid out the same way, as `kn5Options`
    // asks (the car's default skin unless it names another).
    // Existing destination files are kept, never overwritten. Returns the
    // path of the model the engine loads. `progress` hears how far the copy got.
    static std::filesystem::path CopyModelWithSortedReferences(
        const std::filesystem::path& modelPath,
        const std::filesystem::path& targetDirectory,
        const Kn5ImportOptions& kn5Options = {},
        const ImportProgressCallback& progress = {});
};
}
