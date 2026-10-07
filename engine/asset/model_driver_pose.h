#pragma once

#include "model_animation.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace me
{

// The joints a seated driver's pose moves, found by name in a humanoid skeleton (AdvancedSkeleton's
// names, as the AnimateApp characters have them: Root_M, Hip_L, Knee_L, Shoulder_R ...). Index 0 of
// each pair is the character's left side, 1 its right.
struct DriverRig
{
    int32_t root = -1;
    int32_t spine = -1;
    int32_t chest = -1;
    int32_t neck = -1;
    int32_t head = -1;
    std::array<int32_t, 2> hip{-1, -1};
    std::array<int32_t, 2> knee{-1, -1};
    std::array<int32_t, 2> ankle{-1, -1};
    std::array<int32_t, 2> toes{-1, -1};
    std::array<int32_t, 2> scapula{-1, -1};
    std::array<int32_t, 2> shoulder{-1, -1};
    std::array<int32_t, 2> elbow{-1, -1};
    std::array<int32_t, 2> wrist{-1, -1};
    // The forearm's twist joints, which share the elbow's parent rotation and spread the hand's turn
    // along the forearm (-1 where the rig has none).
    std::array<std::array<int32_t, 2>, 2> forearmTwist{{{-1, -1}, {-1, -1}}};
    // Per side, per finger (thumb, index, middle, ring, pinky), its first three joints from the palm.
    std::array<std::array<std::array<int32_t, 3>, 5>, 2> fingers{};
    // The middle finger's base: with the wrist, the hand's length and direction.
    std::array<int32_t, 2> handEnd{-1, -1};
    std::array<int32_t, 2> eye{-1, -1};
};

// The rig when the skeleton has every joint the pose needs (fingers and eyes are optional).
std::optional<DriverRig> FindDriverRig(const ModelSkeleton& skeleton);

// A hand off the wheel, holding something else (the gear lever's knob): where the palm holds it, which
// way the hand points and the palm faces, and how far the hand has gone there from the wheel (0 on
// the wheel, 1 there).
struct DriverHandHold
{
    glm::vec3 grip{0.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    glm::vec3 palmFacing{0.0f, -1.0f, 0.0f};
    float weight = 0.0f;
};

// Where the driver sits and what it holds, in the character's model space (+Z its front, +Y up,
// +X its left), as a car's seat puts it there.
struct DriverPoseInput
{
    // The midpoint of the hip joints (the seat's H-point).
    glm::vec3 hips{0.0f};
    // The pelvis leaned back from upright (degrees), as the seat's back is; the back then bends this
    // much forward from it, towards the wheel.
    float reclineDegrees = 15.0f;
    float leanDegrees = 0.0f;
    // The steering wheel: its centre, its column pointing away from the driver (unit), the radius
    // the hands hold it at, and how far it has turned (radians; positive is clockwise as the driver
    // sees it, a right turn).
    glm::vec3 wheelCenter{0.0f, 0.4f, 0.6f};
    glm::vec3 wheelAxis{0.0f, -0.35f, 0.94f};
    float wheelRadius = 0.18f;
    float wheelTurn = 0.0f;
    // The ankles (left, right), on the pedals, and how far each foot is raised from flat (degrees: up
    // the pedal's slope, less as the foot presses it).
    std::array<glm::vec3, 2> ankles{glm::vec3(0.1f, -0.15f, 0.7f), glm::vec3(-0.1f, -0.15f, 0.7f)};
    std::array<float, 2> footRaiseDegrees{40.0f, 40.0f};
    // A hand on the gear lever instead of the wheel, per side.
    std::array<DriverHandHold, 2> holds{};
    // The head turned towards where the car goes (degrees, positive to the driver's left).
    float headYawDegrees = 0.0f;
    // Shrinks the head (and everything on it, the hair too) away: for a camera at the driver's eyes.
    bool hideHead = false;
};

struct DriverPoseResult
{
    // Between the eyes (between the eye joints, else 0.1 m over the head joint), posed.
    glm::vec3 eyes{0.0f};
    // How far each wrist has to reach for the wheel, as a share of its arm's length (shoulder to
    // elbow to wrist): above 1 the hand falls short of the rim.
    std::array<float, 2> armStretch{0.0f, 0.0f};
};

// The joints' local poses for a driver seated at `input`: the pelvis and back leaned back on the
// seat, the legs bent to the pedals and the hands on the wheel at a quarter to three (two-bone
// inverse kinematics with the knees up and the elbows down), the fingers closed round the rim. The
// hands follow the wheel up to 120 degrees each way and slide round the rim beyond.
void PoseDriver(const ModelSkeleton& skeleton, const DriverRig& rig, const DriverPoseInput& input, std::vector<ModelNodePose>& poses,
                DriverPoseResult* result = nullptr);

// PoseDriver's joint palette.
void EvaluateDriverPalette(const ModelSkeleton& skeleton, const DriverRig& rig, const DriverPoseInput& input, std::vector<glm::mat4>& palette,
                           DriverPoseResult* result = nullptr);
}
