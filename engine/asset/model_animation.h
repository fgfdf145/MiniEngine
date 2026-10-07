#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace me
{

// A glTF model's node hierarchy at rest, the skins its meshes are bound to and the animations that
// move it (glTF skins and animations). The loader bakes every node's transform into its vertices, as
// it does for any model; a skinned submesh's vertices are therefore its bind pose in the model's space,
// and the joint palette (EvaluateJointPalette) moves them from there.
struct ModelSkeletonNode
{
    std::string name;
    // -1 for a scene root.
    int32_t parent = -1;
    // The node's own transform: a matrix where the glTF gives one, else translation, rotation and
    // scale, which animations replace channel by channel.
    bool hasMatrix = false;
    glm::mat4 matrix{1.0f};
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// One skinned node's binding: its skin's joints and inverse bind matrices, and the inverse of the
// node's world transform at rest, which the loader baked into the vertices. A vertex's joint index
// (VertexSkin::joints) indexes these joints; its palette entries start at paletteOffset.
struct ModelSkinBinding
{
    std::vector<int32_t> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
    glm::mat4 bakedInverse{1.0f};
    uint32_t paletteOffset = 0;
};

enum class ModelAnimationPath : uint8_t
{
    Translation,
    Rotation,
    Scale
};

enum class ModelAnimationInterpolation : uint8_t
{
    Linear,
    Step,
    CubicSpline
};

// One glTF channel with its sampler: the keys of one node's translation, rotation or scale. Values
// are xyz for translation and scale and xyzw for rotation; a cubic spline keeps glTF's in-tangent,
// value, out-tangent triples.
struct ModelAnimationChannel
{
    int32_t node = -1;
    ModelAnimationPath path = ModelAnimationPath::Translation;
    ModelAnimationInterpolation interpolation = ModelAnimationInterpolation::Linear;
    std::vector<float> times;
    std::vector<glm::vec4> values;
};

struct ModelAnimationClip
{
    std::string name;
    // The last key's time, in seconds.
    float duration = 0.0f;
    std::vector<ModelAnimationChannel> channels;
};

struct ModelSkeleton
{
    std::vector<ModelSkeletonNode> nodes;
    // Every node index, parents before their children.
    std::vector<int32_t> order;
    std::vector<ModelSkinBinding> bindings;
    std::vector<ModelAnimationClip> clips;
    // The joint palette's length: every binding's joints, one after another.
    uint32_t paletteSize = 0;

    // The index of the clip with this name; -1 for none.
    int32_t FindClip(const std::string& name) const;
    // The node with this name; -1 for none.
    int32_t FindNode(const std::string& name) const;
};

// One node's local transform in a pose: its rest transform, with what an animation (or a procedural
// pose) set. `posed` replaces a node given as a matrix by these; one never posed keeps its matrix.
struct ModelNodePose
{
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    bool posed = false;
};

// Every node at rest.
void RestNodePoses(const ModelSkeleton& skeleton, std::vector<ModelNodePose>& poses);
// Every node as the clip has it at time (seconds, wrapped into its duration); at rest for clip -1 or
// one out of range, and where the clip does not move it.
void EvaluateNodePoses(const ModelSkeleton& skeleton, int32_t clip, float time, std::vector<ModelNodePose>& poses);
// Each node's transform in the model's space for these local poses.
void ComputeNodeWorldMatrices(const ModelSkeleton& skeleton, const std::vector<ModelNodePose>& poses, std::vector<glm::mat4>& world);
// The joint palette for the nodes' model-space transforms: per binding, per joint, the matrix that
// takes a bind pose vertex (in the model's space, as the loader baked it) to its posed place.
void PaletteFromNodeWorldMatrices(const ModelSkeleton& skeleton, const std::vector<glm::mat4>& world, std::vector<glm::mat4>& palette);

// The joint palette of a clip at time. clip -1 or out of range gives the bind pose, every matrix the
// identity. time is in seconds, wrapped into the clip's duration.
void EvaluateJointPalette(const ModelSkeleton& skeleton, int32_t clip, float time, std::vector<glm::mat4>& palette);
}
