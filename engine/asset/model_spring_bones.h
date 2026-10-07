#pragma once

#include "model_animation.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace me
{

// Hair, skirts and other dangling parts that swing as the body moves (spring bones, as VRM's
// SpringBone does it): each joint of such a chain points at its child, whose position is a particle
// carried on by its own inertia, pulled back towards where the animation puts it, weighed down a
// little, kept at the bone's length and pushed out of the body. It works in the model's own frame:
// moving steadily drags nothing behind, while the frame's acceleration (a driver's car braking or
// turning) and gravity act on the chains as forces.
struct SpringBoneSettings
{
    // Pull back towards the animated direction (per second), loss of velocity per step (0 to 1),
    // the weight's pull (metres per second each step), and the particle's radius against the colliders.
    float stiffness = 1.0f;
    float drag = 0.4f;
    float gravity = 0.0f;
    float radius = 0.02f;
};

// A capsule between two joints (a sphere when they are one), its radius in metres.
struct SpringBoneCollider
{
    int32_t from = -1;
    int32_t to = -1;
    float radius = 0.05f;
};

// A flat surface the particles stay on the front of (a seat's cushion and back), in the model's space.
struct SpringBonePlane
{
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

struct SpringBoneSystem
{
    struct Joint
    {
        int32_t node = -1;
        // The child the joint points at.
        int32_t tail = -1;
        SpringBoneSettings settings;
    };
    // Parents before their children.
    std::vector<Joint> joints;
    std::vector<SpringBoneCollider> colliders;

    bool Empty() const
    {
        return joints.empty();
    }
};

// The skeleton's dangling chains, found by their joints' names: hair and bangs, skirts, earrings,
// bows and belt ends (every skinned joint below the first of a chain, which has a child to point at),
// and the body's capsules (AdvancedSkeleton's head, neck, spine, pelvis, legs and arms) they collide with.
SpringBoneSystem BuildSpringBones(const ModelSkeleton& skeleton);

struct SpringBoneState
{
    // The particles, in the model's space.
    std::vector<glm::vec3> current;
    std::vector<glm::vec3> previous;
    bool started = false;
    // The model's origin in the world, and its velocity, for the frame's acceleration.
    glm::vec3 lastOrigin{0.0f};
    glm::vec3 lastVelocity{0.0f};
    int samples = 0;
};

// One step: turns the chains' joints in `poses` (the animated pose, changed in place) to where their
// particles have swung to, for a model placed at modelToWorld. Starts from the animated pose on the
// first step, and again after the model jumps more than a few metres in one step.
void SimulateSpringBones(
    const ModelSkeleton& skeleton,
    const SpringBoneSystem& system,
    SpringBoneState& state,
    std::vector<ModelNodePose>& poses,
    const glm::mat4& modelToWorld,
    float deltaSeconds,
    const std::vector<SpringBonePlane>& planes = {});
}
