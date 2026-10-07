#include "model_spring_bones.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace me
{

namespace
{
// Per kind of chain: hair swings freely, a skirt keeps nearer its shape, the small parts in between.
constexpr SpringBoneSettings kHair{0.9f, 0.35f, 0.25f, 0.015f};
constexpr SpringBoneSettings kSkirt{1.6f, 0.5f, 0.15f, 0.02f};
constexpr SpringBoneSettings kAccessory{1.2f, 0.4f, 0.2f, 0.01f};
// A step longer than this (a stalled frame) is taken as this long; a model that moves farther than this
// in one step has jumped, and its chains start again from the animated pose.
constexpr float kMaxStepSeconds = 1.0f / 30.0f;
constexpr float kJumpMetres = 5.0f;

enum class ChainKind
{
    None,
    Hair,
    Skirt,
    Accessory
};

// By the words of the joint's name, split at underscores (AnimateApp's names: Emi_hair_B_1_0, bang_F_1_1,
// Skirt_3_2, earrings_L, bow_root, belt_L_0; not Elbow_L).
ChainKind ClassifyJoint(const std::string& name)
{
    std::string lower(name.size(), '\0');
    std::transform(name.begin(), name.end(), lower.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    std::vector<std::string> words;
    size_t start = 0;
    while (start <= lower.size())
    {
        const size_t end = std::min(lower.find('_', start), lower.size());
        words.push_back(lower.substr(start, end - start));
        start = end + 1;
    }
    const auto has = [&](const char* word)
    {
        return std::find(words.begin(), words.end(), word) != words.end();
    };
    if (has("hair") || has("bang"))
    {
        return ChainKind::Hair;
    }
    if (has("skirt"))
    {
        return ChainKind::Skirt;
    }
    if (has("earrings") || has("earring") || has("bow") || has("belt") || has("ribbon"))
    {
        return ChainKind::Accessory;
    }
    return ChainKind::None;
}

glm::mat4 LocalMatrix(const ModelSkeletonNode& node, const ModelNodePose& pose)
{
    if (node.hasMatrix && !pose.posed)
    {
        return node.matrix;
    }
    return glm::translate(glm::mat4(1.0f), pose.translation) * glm::mat4_cast(pose.rotation) * glm::scale(glm::mat4(1.0f), pose.scale);
}

glm::quat RotationOf(const glm::mat4& matrix)
{
    const auto unit = [](const glm::vec3& value)
    {
        const float length = glm::length(value);
        return length > 1e-8f ? value / length : value;
    };
    return glm::normalize(glm::quat_cast(glm::mat3(unit(glm::vec3(matrix[0])), unit(glm::vec3(matrix[1])), unit(glm::vec3(matrix[2])))));
}

glm::quat TurnBetween(const glm::vec3& from, const glm::vec3& to)
{
    const glm::vec3 a = glm::normalize(from);
    const glm::vec3 b = glm::normalize(to);
    const float cosine = glm::dot(a, b);
    if (cosine < -0.9999f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

glm::vec3 ClosestOnSegment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 ab = b - a;
    const float squared = glm::dot(ab, ab);
    const float t = squared > 1e-12f ? std::clamp(glm::dot(point - a, ab) / squared, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}
}

SpringBoneSystem BuildSpringBones(const ModelSkeleton& skeleton)
{
    SpringBoneSystem system;
    std::unordered_set<int32_t> skinned;
    for (const ModelSkinBinding& binding : skeleton.bindings)
    {
        skinned.insert(binding.jointNodes.begin(), binding.jointNodes.end());
    }
    std::vector<int32_t> firstChild(skeleton.nodes.size(), -1);
    for (size_t index = skeleton.nodes.size(); index-- > 0;)
    {
        const int32_t parent = skeleton.nodes[index].parent;
        if (parent >= 0)
        {
            firstChild[static_cast<size_t>(parent)] = static_cast<int32_t>(index);
        }
    }
    for (const int32_t node : skeleton.order)
    {
        const ChainKind kind = ClassifyJoint(skeleton.nodes[static_cast<size_t>(node)].name);
        const int32_t tail = firstChild[static_cast<size_t>(node)];
        if (kind == ChainKind::None || tail < 0 || skinned.count(node) == 0 ||
            glm::length(skeleton.nodes[static_cast<size_t>(tail)].translation) < 1e-4f)
        {
            continue;
        }
        SpringBoneSystem::Joint joint;
        joint.node = node;
        joint.tail = tail;
        joint.settings = kind == ChainKind::Hair ? kHair : kind == ChainKind::Skirt ? kSkirt : kAccessory;
        system.joints.push_back(joint);
    }
    if (system.joints.empty())
    {
        return system;
    }

    // The body, as AdvancedSkeleton names it.
    const auto add = [&](const char* from, const char* to, float radius)
    {
        const int32_t a = skeleton.FindNode(from);
        const int32_t b = skeleton.FindNode(to);
        if (a >= 0 && b >= 0)
        {
            system.colliders.push_back(SpringBoneCollider{a, b, radius});
        }
    };
    add("Head_M", "HeadEnd_M", 0.085f);
    add("Neck_M", "Head_M", 0.05f);
    add("Chest_M", "Neck_M", 0.09f);
    add("Spine1_M", "Chest_M", 0.09f);
    add("Root_M", "Spine1_M", 0.1f);
    for (const char* side : {"_L", "_R"})
    {
        const std::string s(side);
        add(("Hip" + s).c_str(), ("Knee" + s).c_str(), 0.07f);
        add(("Knee" + s).c_str(), ("Ankle" + s).c_str(), 0.05f);
        add(("Shoulder" + s).c_str(), ("Elbow" + s).c_str(), 0.045f);
        add(("Elbow" + s).c_str(), ("Wrist" + s).c_str(), 0.04f);
    }
    return system;
}

void SimulateSpringBones(
    const ModelSkeleton& skeleton,
    const SpringBoneSystem& system,
    SpringBoneState& state,
    std::vector<ModelNodePose>& poses,
    const glm::mat4& modelToWorld,
    float deltaSeconds,
    const std::vector<SpringBonePlane>& planes)
{
    if (system.Empty())
    {
        return;
    }
    const float step = std::clamp(deltaSeconds, 0.0f, kMaxStepSeconds);
    std::vector<glm::mat4> model;
    ComputeNodeWorldMatrices(skeleton, poses, model);

    // The particles live in the model's own frame, so that a model moving steadily (a driver in a car at
    // speed) drags nothing behind it; what the frame does that is felt (its acceleration, gravity) acts on
    // them as forces, turned into the frame and taken out of its scale.
    const glm::vec3 origin(modelToWorld[3]);
    const float scale = std::max(std::cbrt(std::abs(glm::determinant(glm::mat3(modelToWorld)))), 1e-6f);
    const glm::mat3 axes = glm::mat3(modelToWorld) / scale;
    const glm::mat3 toModel = glm::transpose(axes);
    const bool jumped = state.started && glm::distance(origin, state.lastOrigin) > kJumpMetres;
    if (!state.started || jumped || state.current.size() != system.joints.size())
    {
        state.current.resize(system.joints.size());
        for (size_t index = 0; index < system.joints.size(); ++index)
        {
            state.current[index] = glm::vec3(model[static_cast<size_t>(system.joints[index].tail)][3]);
        }
        state.previous = state.current;
        state.started = true;
        state.samples = 0;
    }
    glm::vec3 inertia(0.0f);
    if (step > 0.0f)
    {
        const glm::vec3 velocity = (origin - state.lastOrigin) / step;
        if (state.samples >= 2)
        {
            inertia = toModel * (-(velocity - state.lastVelocity) / step) * (step * step) / scale;
        }
        state.samples = std::min(state.samples + 1, 2);
        state.lastVelocity = velocity;
    }
    state.lastOrigin = origin;
    const glm::vec3 down = toModel * glm::vec3(0.0f, -1.0f, 0.0f);

    for (size_t index = 0; index < system.joints.size(); ++index)
    {
        const SpringBoneSystem::Joint& joint = system.joints[index];
        const ModelSkeletonNode& node = skeleton.nodes[static_cast<size_t>(joint.node)];
        const glm::mat4 parent = node.parent >= 0 ? model[static_cast<size_t>(node.parent)] : glm::mat4(1.0f);
        // Its parent may have swung already.
        model[static_cast<size_t>(joint.node)] = parent * LocalMatrix(node, poses[static_cast<size_t>(joint.node)]);
        const glm::mat4& self = model[static_cast<size_t>(joint.node)];
        const glm::vec3 head(self[3]);
        const glm::vec3 animated = glm::vec3(
            self * LocalMatrix(skeleton.nodes[static_cast<size_t>(joint.tail)], poses[static_cast<size_t>(joint.tail)])[3]);
        const float length = glm::distance(head, animated);
        if (length < 1e-5f)
        {
            continue;
        }

        glm::vec3& current = state.current[index];
        glm::vec3& previous = state.previous[index];
        glm::vec3 next = current;
        if (step > 0.0f)
        {
            const SpringBoneSettings& settings = joint.settings;
            next = current + (current - previous) * (1.0f - settings.drag) + inertia + (animated - head) / length * (settings.stiffness * step) +
                   down * (settings.gravity * step);
        }
        next = head + glm::normalize(next - head) * length;

        // Out of the body (the animated body: the chains do not move it), but never farther than the
        // animation itself has it (a pose may put a skirt into the legs), and in front of the planes
        // (which a pose knows nothing of).
        const float radius = joint.settings.radius;
        for (const SpringBoneCollider& collider : system.colliders)
        {
            const glm::vec3 a(model[static_cast<size_t>(collider.from)][3]);
            const glm::vec3 b(model[static_cast<size_t>(collider.to)][3]);
            const glm::vec3 closest = ClosestOnSegment(next, a, b);
            const float animatedDistance = glm::distance(animated, ClosestOnSegment(animated, a, b));
            const float limit = std::min(collider.radius + radius, animatedDistance);
            const glm::vec3 offset = next - closest;
            const float distance = glm::length(offset);
            if (distance < limit && distance > 1e-6f)
            {
                next = closest + offset / distance * limit;
                next = head + glm::normalize(next - head) * length;
            }
        }
        for (const SpringBonePlane& plane : planes)
        {
            const float height = glm::dot(next - plane.point, plane.normal);
            if (height < radius)
            {
                next += plane.normal * (radius - height);
                next = head + glm::normalize(next - head) * length;
            }
        }
        if (step > 0.0f)
        {
            previous = current;
            current = next;
        }

        // The joint turned to point at its particle.
        const glm::quat turn = TurnBetween(animated - head, next - head);
        const glm::quat parentRotation = node.parent >= 0 ? RotationOf(parent) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        ModelNodePose& pose = poses[static_cast<size_t>(joint.node)];
        pose.rotation = glm::normalize(glm::conjugate(parentRotation) * (turn * RotationOf(self)));
        pose.posed = true;
        model[static_cast<size_t>(joint.node)] = parent * LocalMatrix(node, pose);
    }
}
}
