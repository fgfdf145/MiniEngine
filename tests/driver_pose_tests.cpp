#include "imgui_software_raster.h"

#include <engine/asset/model_cache.h>
#include <engine/asset/model_driver_pose.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/model_spring_bones.h>
#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/services/vehicle_drive_service.h>
#include <engine/editor/services/vehicle_driver_service.h>
#include <engine/editor/services/vehicle_gear_shift.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/scene_panel.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
// Shorter than the time the left foot stays on the clutch.
constexpr float kFootHoldTestSeconds = 0.1f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

int32_t AddNode(ModelSkeleton& skeleton, const std::string& name, int32_t parent, const glm::vec3& translation)
{
    ModelSkeletonNode node;
    node.name = name;
    node.parent = parent;
    node.translation = translation;
    skeleton.nodes.push_back(node);
    const int32_t index = static_cast<int32_t>(skeleton.nodes.size()) - 1;
    skeleton.order.push_back(index);
    return index;
}

// A 1.6 m humanoid in a T-pose, facing +Z, with AdvancedSkeleton's joint names; +X is its left.
ModelSkeleton MakeHumanoid()
{
    ModelSkeleton skeleton;
    const int32_t root = AddNode(skeleton, "Root_M", -1, glm::vec3(0.0f, 1.0f, 0.0f));
    const int32_t spine = AddNode(skeleton, "Spine1_M", root, glm::vec3(0.0f, 0.15f, 0.0f));
    const int32_t chest = AddNode(skeleton, "Chest_M", spine, glm::vec3(0.0f, 0.12f, 0.0f));
    const int32_t neck = AddNode(skeleton, "Neck_M", chest, glm::vec3(0.0f, 0.17f, 0.0f));
    const int32_t head = AddNode(skeleton, "Head_M", neck, glm::vec3(0.0f, 0.1f, 0.0f));
    AddNode(skeleton, "Eye_L", head, glm::vec3(0.03f, 0.03f, 0.08f));
    AddNode(skeleton, "Eye_R", head, glm::vec3(-0.03f, 0.03f, 0.08f));
    for (const float side : {1.0f, -1.0f})
    {
        const std::string suffix = side > 0.0f ? "_L" : "_R";
        const int32_t hip = AddNode(skeleton, "Hip" + suffix, root, glm::vec3(0.07f * side, -0.04f, 0.0f));
        const int32_t knee = AddNode(skeleton, "Knee" + suffix, hip, glm::vec3(0.0f, -0.41f, 0.0f));
        const int32_t ankle = AddNode(skeleton, "Ankle" + suffix, knee, glm::vec3(0.0f, -0.45f, 0.0f));
        AddNode(skeleton, "Toes" + suffix, ankle, glm::vec3(0.0f, -0.05f, 0.12f));
        const int32_t scapula = AddNode(skeleton, "Scapula" + suffix, chest, glm::vec3(0.04f * side, 0.12f, 0.0f));
        const int32_t shoulder = AddNode(skeleton, "Shoulder" + suffix, scapula, glm::vec3(0.07f * side, 0.0f, 0.0f));
        const int32_t elbow = AddNode(skeleton, "Elbow" + suffix, shoulder, glm::vec3(0.24f * side, 0.0f, 0.0f));
        const int32_t wrist = AddNode(skeleton, "Wrist" + suffix, elbow, glm::vec3(0.24f * side, 0.0f, 0.0f));
        const int32_t middle1 = AddNode(skeleton, "MiddleFinger1" + suffix, wrist, glm::vec3(0.08f * side, 0.0f, 0.0f));
        const int32_t middle2 = AddNode(skeleton, "MiddleFinger2" + suffix, middle1, glm::vec3(0.045f * side, 0.0f, 0.0f));
        const int32_t middle3 = AddNode(skeleton, "MiddleFinger3" + suffix, middle2, glm::vec3(0.03f * side, 0.0f, 0.0f));
        AddNode(skeleton, "MiddleFinger4" + suffix, middle3, glm::vec3(0.025f * side, 0.0f, 0.0f));
        // The thumb off the index side of the palm (forward, +Z), pointing out and forward.
        const int32_t thumb1 = AddNode(skeleton, "ThumbFinger1" + suffix, wrist, glm::vec3(0.02f * side, -0.01f, 0.02f));
        const int32_t thumb2 = AddNode(skeleton, "ThumbFinger2" + suffix, thumb1, glm::vec3(0.025f * side, 0.0f, 0.02f));
        const int32_t thumb3 = AddNode(skeleton, "ThumbFinger3" + suffix, thumb2, glm::vec3(0.02f * side, 0.0f, 0.015f));
        AddNode(skeleton, "ThumbFinger4" + suffix, thumb3, glm::vec3(0.018f * side, 0.0f, 0.012f));
    }
    return skeleton;
}

std::vector<glm::vec3> WorldPositions(const ModelSkeleton& skeleton, const std::vector<ModelNodePose>& poses)
{
    std::vector<glm::mat4> world;
    ComputeNodeWorldMatrices(skeleton, poses, world);
    std::vector<glm::vec3> positions;
    for (const glm::mat4& matrix : world)
    {
        positions.push_back(glm::vec3(matrix[3]));
    }
    return positions;
}

void SeatedPoseReachesItsTargets()
{
    const ModelSkeleton skeleton = MakeHumanoid();
    const std::optional<DriverRig> rig = FindDriverRig(skeleton);
    Require(rig.has_value(), "the humanoid's rig was not found");

    DriverPoseInput input;
    input.hips = glm::vec3(0.0f, 0.5f, 0.0f);
    input.reclineDegrees = 15.0f;
    input.wheelCenter = glm::vec3(0.0f, 0.8f, 0.38f);
    input.wheelAxis = glm::normalize(glm::vec3(0.0f, -0.35f, 0.94f));
    input.wheelRadius = 0.18f;
    input.ankles = {glm::vec3(0.1f, 0.3f, 0.65f), glm::vec3(-0.1f, 0.3f, 0.65f)};
    std::vector<ModelNodePose> poses;
    DriverPoseResult result;
    PoseDriver(skeleton, *rig, input, poses, &result);
    const std::vector<glm::vec3> at = WorldPositions(skeleton, poses);

    const glm::vec3 hips = (at[static_cast<size_t>(rig->hip[0])] + at[static_cast<size_t>(rig->hip[1])]) * 0.5f;
    Require(glm::distance(hips, input.hips) < 1e-3f, "the hips are not on the seat");
    for (size_t side = 0; side < 2; ++side)
    {
        const glm::vec3 ankle = at[static_cast<size_t>(rig->ankle[side])];
        Require(glm::distance(ankle, input.ankles[side]) < 2e-3f, "an ankle missed its pedal");
        // The knee above the line from the hip to the ankle: bent up, not down.
        const glm::vec3 hip = at[static_cast<size_t>(rig->hip[side])];
        const glm::vec3 knee = at[static_cast<size_t>(rig->knee[side])];
        const float along = glm::dot(knee - hip, glm::normalize(ankle - hip));
        const glm::vec3 offLine = knee - hip - glm::normalize(ankle - hip) * along;
        Require(offLine.y > 0.05f, "a knee bends down");

        // The wrist behind the rim at a quarter to three, the elbow below the shoulder and the wrist.
        const glm::vec3 wrist = at[static_cast<size_t>(rig->wrist[side])];
        Require(std::abs(glm::distance(wrist, input.wheelCenter) - 0.2f) < 0.06f, "a hand is not at the rim");
        Require((side == 0) == (wrist.x > 0.0f), "a hand holds the wrong side of the wheel");
        const glm::vec3 elbow = at[static_cast<size_t>(rig->elbow[side])];
        const glm::vec3 shoulder = at[static_cast<size_t>(rig->shoulder[side])];
        Require(elbow.y < shoulder.y && elbow.y < wrist.y, "an elbow is not down");
        Require(result.armStretch[side] < 1.0f, "an arm cannot reach the wheel");

        // The middle finger closed round the rim: its joints and tip on the rim's surface, not in it and
        // not off it.
        const auto fromRimSurface = [&](const glm::vec3& point)
        {
            const glm::vec3 offset = point - input.wheelCenter;
            const glm::vec3 inPlane = offset - input.wheelAxis * glm::dot(offset, input.wheelAxis);
            return glm::distance(point, input.wheelCenter + glm::normalize(inPlane) * input.wheelRadius) - input.wheelTubeRadius;
        };
        for (size_t joint = 1; joint < 4; ++joint)
        {
            const float gap = fromRimSurface(at[static_cast<size_t>(rig->fingers[side][2][joint])]);
            Require(gap > -0.005f && gap < 0.02f, "a finger does not close on the rim");
        }
        const float thumbGap = fromRimSurface(at[static_cast<size_t>(rig->fingers[side][0][3])]);
        Require(thumbGap > -0.005f && thumbGap < 0.02f, "a thumb does not lie on the rim");
    }
    Require(result.eyes.y > at[static_cast<size_t>(rig->head)].y, "the eyes are not on the head");
}

void HandsHoldWhereTheyAreTold()
{
    const ModelSkeleton skeleton = MakeHumanoid();
    const DriverRig rig = *FindDriverRig(skeleton);
    DriverPoseInput input;
    input.hips = glm::vec3(0.0f, 0.5f, 0.0f);
    input.wheelCenter = glm::vec3(0.0f, 0.8f, 0.38f);
    input.wheelAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    std::vector<ModelNodePose> poses;
    PoseDriver(skeleton, rig, input, poses);
    const glm::vec3 leftLevel = WorldPositions(skeleton, poses)[static_cast<size_t>(rig.wrist[0])];
    // Turned right with the wheel, the left hand goes up over the top, the right one down.
    input.gripAngles = {glm::radians(-30.0f), glm::radians(150.0f)};
    PoseDriver(skeleton, rig, input, poses);
    const std::vector<glm::vec3> turned = WorldPositions(skeleton, poses);
    Require(turned[static_cast<size_t>(rig.wrist[0])].y > leftLevel.y + 0.08f, "the left hand did not go up in a right turn");
    Require(turned[static_cast<size_t>(rig.wrist[1])].y < leftLevel.y - 0.08f, "the right hand did not go down in a right turn");
}

void HiddenHeadShrinks()
{
    const ModelSkeleton skeleton = MakeHumanoid();
    const DriverRig rig = *FindDriverRig(skeleton);
    DriverPoseInput input;
    input.hideHead = true;
    std::vector<ModelNodePose> poses;
    PoseDriver(skeleton, rig, input, poses);
    Require(poses[static_cast<size_t>(rig.head)].scale.x < 0.01f, "the head was not hidden");
}

void HeldHandGoesToTheKnob()
{
    const ModelSkeleton skeleton = MakeHumanoid();
    const DriverRig rig = *FindDriverRig(skeleton);
    DriverPoseInput input;
    input.hips = glm::vec3(0.0f, 0.5f, 0.0f);
    input.wheelCenter = glm::vec3(0.0f, 0.8f, 0.38f);
    input.holds[0].grip = glm::vec3(0.3f, 0.55f, 0.25f);
    input.holds[0].direction = glm::normalize(glm::vec3(0.0f, -0.4f, 1.0f));
    input.holds[0].palmFacing = glm::normalize(glm::vec3(0.0f, -1.0f, -0.4f));
    input.holds[0].weight = 1.0f;
    std::vector<ModelNodePose> poses;
    PoseDriver(skeleton, rig, input, poses);
    const std::vector<glm::vec3> at = WorldPositions(skeleton, poses);
    const glm::vec3 wrist = at[static_cast<size_t>(rig.wrist[0])];
    // The wrist behind the grip, off the palm's side; the right hand stays on the wheel.
    Require(glm::distance(wrist, input.holds[0].grip) < 0.09f && wrist.z < input.holds[0].grip.z, "the hand did not go to the knob");
    Require(std::abs(glm::distance(at[static_cast<size_t>(rig.wrist[1])], input.wheelCenter) - 0.2f) < 0.06f, "the other hand left the wheel");
}

void GearLeverGoesThroughTheGate()
{
    Require(GearGatePosition(1) == glm::vec2(1.0f, 1.0f) && GearGatePosition(2) == glm::vec2(1.0f, -1.0f), "first and second are not left");
    Require(GearGatePosition(3) == glm::vec2(0.0f, 1.0f) && GearGatePosition(6) == glm::vec2(-1.0f, -1.0f), "the gate's planes are wrong");
    Require(GearGatePosition(0) == glm::vec2(0.0f) && GearGatePosition(-1).x > 1.5f, "neutral or reverse is wrong");

    VehicleGearShift shift;
    shift.from = shift.to = 2;
    StartGearShift(shift, 3);
    Require(shift.seconds == 0.0f && GearShiftHandWeight(shift) == 0.0f, "a shift does not start with the hand on the wheel");
    AdvanceGearShift(shift, kGearShiftReachSeconds);
    Require(GearShiftHandWeight(shift) == 1.0f && GearLeverGatePosition(shift) == GearGatePosition(2), "the hand reaches the lever before it moves");
    // Halfway through the move the lever is on the gate's middle line, between the planes.
    AdvanceGearShift(shift, kGearShiftMoveSeconds * 0.5f);
    const glm::vec2 middle = GearLeverGatePosition(shift);
    Require(std::abs(middle.y) < 1e-4f && middle.x > -1e-4f && middle.x < 1.0f + 1e-4f, "the lever does not go by way of neutral");
    AdvanceGearShift(shift, kGearShiftMoveSeconds * 0.5f);
    Require(glm::distance(GearLeverGatePosition(shift), GearGatePosition(3)) < 1e-4f, "the lever did not reach third");
    AdvanceGearShift(shift, kGearShiftReturnSeconds);
    Require(GearShiftHandWeight(shift) < 1e-3f, "the hand did not go back to the wheel");

    // Another change while the hand comes back takes it to the lever again from where it is.
    shift.seconds = kGearShiftReachSeconds + kGearShiftMoveSeconds + kGearShiftReturnSeconds * 0.5f;
    const float before = GearShiftHandWeight(shift);
    StartGearShift(shift, 4);
    Require(std::abs(GearShiftHandWeight(shift) - before) < 0.2f && shift.from == 3, "a quick second change jumps the hand");

    // The lever turns about its pivot, its knob moved forward for an odd gear.
    const ModelGearLever lever{glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.65f, 0.0f)};
    const glm::vec3 knob = glm::vec3(GearLeverTransform(lever, GearGatePosition(1), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) * glm::vec4(lever.knob, 1.0f));
    Require(knob.z > 0.04f && knob.x > 0.03f && std::abs(glm::distance(knob, lever.pivot) - 0.15f) < 1e-4f, "the knob is not in first");
}

void FeetWorkThePedals()
{
    VehicleDriverFeet feet;
    for (int frame = 0; frame < 30; ++frame)
    {
        UpdateDriverFeet(feet, 0.0f, 0.8f, 0.0f, 1.0f / 60.0f);
    }
    Require(feet.rightOnBrake == 1.0f && std::abs(feet.brake - 0.8f) < 0.01f && feet.throttle < 0.01f, "the right foot did not brake");
    for (int frame = 0; frame < 30; ++frame)
    {
        UpdateDriverFeet(feet, 0.0f, 0.0f, 0.0f, 1.0f / 60.0f);
    }
    Require(feet.rightOnBrake == 1.0f && feet.brake < 0.01f, "the right foot left the brake with nothing to do");
    for (int frame = 0; frame < 30; ++frame)
    {
        UpdateDriverFeet(feet, 1.0f, 0.0f, 1.0f, 1.0f / 60.0f);
    }
    Require(feet.rightOnBrake == 0.0f && feet.throttle > 0.95f, "the right foot did not go back to the accelerator");
    Require(feet.leftOnClutch == 1.0f && feet.clutch > 0.95f, "the left foot did not press the clutch");
    // It stays on the clutch a moment after it closes, then goes back to the rest.
    UpdateDriverFeet(feet, 1.0f, 0.0f, 0.0f, kFootHoldTestSeconds);
    Require(feet.leftOnClutch == 1.0f, "the left foot left the clutch at once");
    for (int frame = 0; frame < 60; ++frame)
    {
        UpdateDriverFeet(feet, 1.0f, 0.0f, 0.0f, 1.0f / 60.0f);
    }
    Require(feet.leftOnClutch == 0.0f && feet.clutch < 0.01f, "the left foot did not go back to the rest");
}

VehicleDriverSeat TestSeat()
{
    VehicleDriverSeat seat;
    seat.wheelCenter = glm::vec3(0.0f, 0.8f, 0.4f);
    seat.wheelAxis = glm::normalize(glm::vec3(0.0f, -0.35f, 0.94f));
    seat.wheelRadius = 0.18f;
    return seat;
}

float Wrapped(float angle)
{
    return std::remainder(angle, glm::two_pi<float>());
}

void HandsGoHandOverHand()
{
    const VehicleDriverSeat seat = TestSeat();
    VehicleDriverHands hands;
    UpdateDriverHands(hands, seat, 0.0f, 1.0f / 60.0f);
    Require(std::abs(hands.grips[0] + glm::half_pi<float>()) < 1e-4f && std::abs(hands.grips[1] - glm::half_pi<float>()) < 1e-4f,
            "the hands do not start at a quarter to three");
    // A turn and a half to the right at a full turn a second: the hands take turns letting go, never
    // both at once, never far past their reach, and they cross.
    int regrips = 0;
    bool crossed = false;
    std::array<bool, 2> wasMoving{};
    float turn = 0.0f;
    for (int frame = 0; frame < 90; ++frame)
    {
        turn += glm::two_pi<float>() / 60.0f;
        UpdateDriverHands(hands, seat, turn, 1.0f / 60.0f);
        Require(!(hands.moving[0] && hands.moving[1]), "both hands let go");
        const float left = Wrapped(hands.grips[0] + turn);
        const float right = Wrapped(hands.grips[1] + turn);
        for (size_t side = 0; side < 2; ++side)
        {
            const bool letGo = hands.moving[side] && !wasMoving[side];
            regrips += letGo ? 1 : 0;
            // Crossed: the right hand reaches over to take hold left of the left hand, on the upper half.
            crossed = crossed || (letGo && side == 1 && right < left && right < 0.0f);
            wasMoving[side] = hands.moving[side];
        }
        Require(hands.moving[0] || left < glm::radians(140.0f), "the left hand held on far past its reach");
        Require(hands.moving[1] || right > glm::radians(-140.0f), "the right hand held on far past its reach");
    }
    Require(regrips >= 3, "the hands did not go hand over hand");
    Require(crossed, "the arms never crossed");
    // Back to the middle and held there, the hands go back to a quarter to three.
    for (int frame = 0; frame < 240; ++frame)
    {
        turn += (0.0f - turn) * 0.2f;
        UpdateDriverHands(hands, seat, turn, 1.0f / 60.0f);
    }
    for (int frame = 0; frame < 120; ++frame)
    {
        UpdateDriverHands(hands, seat, turn, 1.0f / 60.0f);
    }
    Require(std::abs(Wrapped(hands.grips[0] + turn + glm::half_pi<float>())) < 0.05f &&
                std::abs(Wrapped(hands.grips[1] + turn - glm::half_pi<float>())) < 0.05f,
            "the hands did not go back to a quarter to three");
}

void BodySwaysWithTheCar()
{
    // Braking at 8 m/s^2 in a straight line leans the body forward; a right turn leans it left.
    VehicleDriverSway braking;
    glm::vec3 position(0.0f);
    float speed = 30.0f;
    for (int frame = 0; frame < 60; ++frame)
    {
        speed -= 8.0f / 60.0f;
        position.z += speed / 60.0f;
        UpdateDriverSway(braking, glm::translate(glm::mat4(1.0f), position), glm::vec3(0.0f), 1.0f / 60.0f);
    }
    Require(braking.lean.y > 3.0f && std::abs(braking.lean.x) < 0.5f, "braking did not lean the body forward");

    // Round a 50 m circle to the right at 20 m/s (8 m/s^2 inwards), the car facing along its way.
    VehicleDriverSway turning;
    for (int frame = 0; frame < 90; ++frame)
    {
        const float angle = 20.0f / 50.0f * static_cast<float>(frame) / 60.0f;
        // Turning right is turning about -Y; the centre is to the car's right (-X).
        const glm::mat4 rotation = glm::mat4_cast(glm::angleAxis(-angle, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 centre(-50.0f, 0.0f, 0.0f);
        const glm::vec3 onCircle = centre + glm::vec3(rotation * glm::vec4(50.0f, 0.0f, 0.0f, 0.0f));
        UpdateDriverSway(turning, glm::translate(glm::mat4(1.0f), onCircle) * rotation, glm::vec3(0.0f), 1.0f / 60.0f);
    }
    Require(turning.lean.x > 3.0f, "a right turn did not lean the body to the left");

    // The car's body rolling on its springs about its origin, standing still: felt a metre up, the body
    // sways; felt at the origin, it hardly does.
    VehicleDriverSway high;
    VehicleDriverSway low;
    float highMost = 0.0f;
    float lowMost = 0.0f;
    for (int frame = 0; frame < 120; ++frame)
    {
        const float roll = glm::radians(3.0f) * std::sin(glm::two_pi<float>() * 1.5f * static_cast<float>(frame) / 60.0f);
        const glm::mat4 body = glm::mat4_cast(glm::angleAxis(roll, glm::vec3(0.0f, 0.0f, 1.0f)));
        UpdateDriverSway(high, body, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f / 60.0f);
        UpdateDriverSway(low, body, glm::vec3(0.0f), 1.0f / 60.0f);
        highMost = std::max(highMost, std::abs(high.lean.x));
        lowMost = std::max(lowMost, std::abs(low.lean.x));
    }
    Require(highMost > 1.0f && highMost > lowMost * 2.0f, "the body did not sway with the car's roll");
}

void HairSwings()
{
    // A three-joint hair strand under a head, skinned, hanging down.
    ModelSkeleton skeleton;
    const int32_t head = AddNode(skeleton, "Head_M", -1, glm::vec3(0.0f, 1.5f, 0.0f));
    const int32_t first = AddNode(skeleton, "Emi_hair_B_1_0", head, glm::vec3(0.0f, 0.0f, -0.1f));
    const int32_t second = AddNode(skeleton, "Emi_hair_B_1_1", first, glm::vec3(0.0f, -0.1f, 0.0f));
    const int32_t third = AddNode(skeleton, "Emi_hair_B_1_2", second, glm::vec3(0.0f, -0.1f, 0.0f));
    AddNode(skeleton, "Emi_hair_B_1_3", third, glm::vec3(0.0f, -0.1f, 0.0f));
    ModelSkinBinding binding;
    for (int32_t node = 0; node < static_cast<int32_t>(skeleton.nodes.size()); ++node)
    {
        binding.jointNodes.push_back(node);
        binding.inverseBindMatrices.push_back(glm::mat4(1.0f));
    }
    skeleton.bindings.push_back(binding);
    skeleton.paletteSize = static_cast<uint32_t>(binding.jointNodes.size());
    const SpringBoneSystem system = BuildSpringBones(skeleton);
    Require(system.joints.size() == 3, "the strand's joints were not found");

    // The head carried forward and stopped hard: the strand swings on forward, its bones keep their length.
    SpringBoneState state;
    std::vector<ModelNodePose> poses;
    float z = 0.0f;
    float tipForward = 0.0f;
    for (int frame = 0; frame < 40; ++frame)
    {
        z += frame < 20 ? 0.2f : 0.0f;
        RestNodePoses(skeleton, poses);
        SimulateSpringBones(skeleton, system, state, poses, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z)), 1.0f / 60.0f);
        std::vector<glm::mat4> world;
        ComputeNodeWorldMatrices(skeleton, poses, world);
        const glm::vec3 root(world[static_cast<size_t>(first)][3]);
        const glm::vec3 tip(world.back()[3]);
        Require(std::abs(glm::distance(glm::vec3(world[static_cast<size_t>(second)][3]), root) - 0.1f) < 1e-3f, "a hair bone stretched");
        if (frame > 20)
        {
            tipForward = std::max(tipForward, tip.z - root.z);
        }
    }
    Require(tipForward > 0.05f, "the strand did not swing on when the head stopped");

    // The frame pitching back and forth about its origin, which stays put: the strand, a metre and a half
    // up, swings in it.
    SpringBoneState pitched;
    float swing = 0.0f;
    for (int frame = 0; frame < 120; ++frame)
    {
        const float pitch = glm::radians(4.0f) * std::sin(glm::two_pi<float>() * 1.5f * static_cast<float>(frame) / 60.0f);
        RestNodePoses(skeleton, poses);
        SimulateSpringBones(skeleton, system, pitched, poses, glm::mat4_cast(glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f))), 1.0f / 60.0f);
        std::vector<glm::mat4> world;
        ComputeNodeWorldMatrices(skeleton, poses, world);
        swing = std::max(swing, std::abs(world.back()[3].z - world[static_cast<size_t>(first)][3].z));
    }
    Require(swing > 0.02f, "the strand did not swing with the frame's pitch");
}

void Print(const char* name, const glm::vec3& value)
{
    std::printf("  %-18s (%7.3f, %7.3f, %7.3f)\n", name, value.x, value.y, value.z);
}

// The Inspector's Driver section for a skinned model with a car in the scene: the car is offered, and
// with MINIENGINE_UI_SNAPSHOT_DIR set the panel is written there as inspector_driver.png.
void InspectorOffersTheCar()
{
    auto driverModel = std::make_shared<LoadedModelData>();
    {
        auto skeleton = std::make_shared<ModelSkeleton>(MakeHumanoid());
        ModelSkinBinding binding;
        binding.jointNodes = {0};
        binding.inverseBindMatrices = {glm::mat4(1.0f)};
        skeleton->bindings.push_back(binding);
        skeleton->paletteSize = 1;
        driverModel->skeleton = skeleton;
    }
    auto carModel = std::make_shared<LoadedModelData>();
    carModel->steeringWheel = ModelSteeringWheel{glm::vec3(0.4f, 0.8f, -0.4f), glm::vec3(0.0f, -0.36f, -0.93f)};
    ModelCache::Store("test/driver.glb", driverModel);
    ModelCache::Store("test/car.gltf", carModel);

    EditorScene scene;
    SerializedEntityData car;
    car.entityUuid = "car-uuid";
    car.tagName = "Skyline";
    car.modelSourcePath = "test/car.gltf";
    scene.CreateEntity(car);
    SerializedEntityData driver;
    driver.entityUuid = "driver-uuid";
    driver.tagName = "Yuki";
    driver.modelSourcePath = "test/driver.glb";
    driver.driverVehicleUuid = "car-uuid";
    const entt::entity driverEntity = scene.CreateEntity(driver);
    scene.SetSelectedEntity(driverEntity);

    Camera camera;
    ViewportMatrices matrices;
    EditorFrameInput frame;
    EditorUiFrameResult result;
    EditorSharedState state;
    EditorStyle style;
    EditorWindowManager windows;
    CommandRegistry commands;
    windows.Register<ScenePanel>();
    constexpr int kWidth = 480;
    constexpr int kHeight = 1100;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    for (int index = 0; index < 3; ++index)
    {
        io.DeltaTime = 1.0f / 60.0f;
        result = EditorUiFrameResult{};
        EditorContext context{scene, camera, matrices, frame, result, state, style, windows, commands};
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        windows.TickAndDraw(context, false);
        ImGui::Render();
        test::ServeTextures(*ImGui::GetDrawData());
    }
    if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
    {
        std::filesystem::create_directories(folder);
        test::WritePng(test::Rasterise(*ImGui::GetDrawData(), kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / "inspector_driver.png");
    }
    Require(scene.GetModel(driverEntity).driverVehicleUuid == "car-uuid", "the scene lost the driver's car");

    // The seat survives a save and a load.
    scene.EditModel(driverEntity).driverSeatOffset = glm::vec3(0.01f, -0.02f, 0.03f);
    scene.EditModel(driverEntity).springBones = false;
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "miniengine_driver_pose_tests.yaml";
    SaveEditorSceneDataToFile(scene.CaptureSceneData(), file.string());
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(file.string());
    std::filesystem::remove(file);
    bool found = false;
    for (const SerializedEntityData& entity : loaded.entities)
    {
        if (entity.entityUuid == "driver-uuid")
        {
            found = entity.driverVehicleUuid == "car-uuid" && glm::distance(entity.driverSeatOffset, glm::vec3(0.01f, -0.02f, 0.03f)) < 1e-6f &&
                    !entity.modelSpringBones;
        }
        else if (entity.entityUuid == "car-uuid")
        {
            Require(entity.driverVehicleUuid.empty(), "the car was saved as a driver");
            Require(entity.modelSpringBones, "the hair and skirt physics was off by default");
        }
    }
    Require(found, "the driver's seat or its physics switch was not saved and loaded");
}

// With MINIENGINE_DRIVER_MODEL and MINIENGINE_DRIVER_CAR set (a character's and a car's glTF), fits the
// character to the car's seat and prints where its joints go.
void PrintRealFit()
{
    const char* const driverPath = std::getenv("MINIENGINE_DRIVER_MODEL");
    const char* const carPath = std::getenv("MINIENGINE_DRIVER_CAR");
    if (driverPath == nullptr || carPath == nullptr)
    {
        return;
    }
    const LoadedModelData driver = ModelLoader::LoadModel(driverPath);
    const LoadedModelData car = ModelLoader::LoadModel(carPath);
    Require(driver.skeleton != nullptr, "the driver has no skeleton");
    const std::optional<DriverRig> rig = FindDriverRig(*driver.skeleton);
    Require(rig.has_value(), "the driver has no humanoid rig");
    const glm::quat vehicleToModel = VehicleDriveService::VehicleToModelRotation(
        car.wheelRig.has_value() ? VehicleDriveService::ModelFrontFromWheels(*car.wheelRig) : VehicleModelFront::NegativeZ);
    const std::optional<VehicleDriverSeat> seat = FitDriverSeat(car, vehicleToModel, *driver.skeleton, *rig);
    Require(seat.has_value(), "no seat");
    const float turn = std::getenv("MINIENGINE_DRIVER_TURN") != nullptr ? glm::radians(static_cast<float>(std::atof(std::getenv("MINIENGINE_DRIVER_TURN")))) : 0.0f;
    std::vector<ModelNodePose> poses;
    DriverPoseResult result;
    const DriverPoseInput input = DriverPoseFromSeat(*seat, glm::vec3(0.0f), turn, false);
    PoseDriver(*driver.skeleton, *rig, input, poses, &result);
    const std::vector<glm::vec3> at = WorldPositions(*driver.skeleton, poses);
    std::printf("Fit (vehicle space): slide %.2f, recline %.1f, wheel r %.3f, arm stretch %.2f %.2f\n", seat->slide, seat->reclineDegrees,
                seat->wheelRadius, result.armStretch[0], result.armStretch[1]);
    Print("car eyes", seat->carEyes);
    if (car.gearLever.has_value())
    {
        Print("lever pivot", glm::conjugate(vehicleToModel) * car.gearLever->pivot);
        Print("lever knob", glm::conjugate(vehicleToModel) * car.gearLever->knob);
        const glm::vec3 first = glm::vec3(GearLeverTransform(*car.gearLever, GearGatePosition(1), vehicleToModel) * glm::vec4(car.gearLever->knob, 1.0f));
        Print("knob in first", glm::conjugate(vehicleToModel) * first);
    }
    Print("eyes", result.eyes);
    Print("wheel", seat->wheelCenter);
    Print("hips target", seat->hips);
    for (size_t side = 0; side < 2; ++side)
    {
        std::printf(" side %zu\n", side);
        Print("hip", at[static_cast<size_t>(rig->hip[side])]);
        Print("knee", at[static_cast<size_t>(rig->knee[side])]);
        Print("ankle", at[static_cast<size_t>(rig->ankle[side])]);
        Print("ankle target", input.ankles[side]);
        Print("toes", at[static_cast<size_t>(rig->toes[side])]);
        Print("shoulder", at[static_cast<size_t>(rig->shoulder[side])]);
        Print("elbow", at[static_cast<size_t>(rig->elbow[side])]);
        Print("wrist", at[static_cast<size_t>(rig->wrist[side])]);
        // Each hand joint's distance from the rim's middle line (a circle round the column).
        const auto fromRim = [&](const glm::vec3& point)
        {
            const glm::vec3 offset = point - seat->wheelCenter;
            const glm::vec3 inPlane = offset - seat->wheelAxis * glm::dot(offset, seat->wheelAxis);
            const glm::vec3 onRim = seat->wheelCenter + glm::normalize(inPlane) * seat->wheelRadius;
            return glm::distance(point, onRim);
        };
        std::printf("  from the rim's middle (rim %.3f): wrist %.3f, knuckle %.3f, middle finger %.3f %.3f, tip %.3f\n", seat->wheelTubeRadius,
                    fromRim(at[static_cast<size_t>(rig->wrist[side])]), fromRim(at[static_cast<size_t>(rig->fingers[side][2][0])]),
                    fromRim(at[static_cast<size_t>(rig->fingers[side][2][1])]), fromRim(at[static_cast<size_t>(rig->fingers[side][2][2])]),
                    fromRim(at[static_cast<size_t>(rig->fingers[side][2][3])]));
        std::printf("  thumb from the rim's middle: %.3f %.3f %.3f\n", fromRim(at[static_cast<size_t>(rig->fingers[side][0][1])]),
                    fromRim(at[static_cast<size_t>(rig->fingers[side][0][2])]), fromRim(at[static_cast<size_t>(rig->fingers[side][0][3])]));
    }
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
    try
    {
        SeatedPoseReachesItsTargets();
        HandsHoldWhereTheyAreTold();
        HiddenHeadShrinks();
        HeldHandGoesToTheKnob();
        GearLeverGoesThroughTheGate();
        FeetWorkThePedals();
        HandsGoHandOverHand();
        BodySwaysWithTheCar();
        HairSwings();
        InspectorOffersTheCar();
        PrintRealFit();
    }
    catch (const std::exception& error)
    {
        std::cerr << "driver pose tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "driver pose tests passed\n";
    return 0;
}
