#include "model_driver_pose.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace me
{

namespace
{
constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
constexpr glm::vec3 kLeft{1.0f, 0.0f, 0.0f};

// The hands: the wheel held at a quarter to three, followed this far each way before the hands slide
// round the rim; the wrist behind the rim and outside it, the palm round it.
constexpr float kGripAboveLevelDegrees = 0.0f;
constexpr float kMaxHandTurnDegrees = 120.0f;
constexpr float kWristBehindRim = 0.045f;
constexpr float kWristOutsideRim = 0.03f;
// The fingers' bend at their three joints from the palm, closed round a rim; the thumb lies along it.
constexpr std::array<float, 3> kFingerCurlDegrees{45.0f, 70.0f, 45.0f};
constexpr std::array<float, 3> kThumbCurlDegrees{10.0f, 20.0f, 20.0f};
// The feet: the line from the ankle to the ball of the foot raised this much from where the rest pose
// has it, onto a pedal's slope.
constexpr float kFootRaiseDegrees = 40.0f;
// The neck and head take back this share of the back's recline each, so the eyes look at the road.
constexpr float kNeckUprightShare = 0.5f;
constexpr float kHeadUprightShare = 0.4f;
// The shoulder blades bring the shoulders forward by up to this much when the arms are nearly straight.
constexpr float kMaxShoulderReachDegrees = 15.0f;
// A hidden head is shrunk to this scale about its joint.
constexpr float kHiddenHeadScale = 1e-3f;

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float length = glm::length(value);
    return length > 1e-6f ? value / length : fallback;
}

// Any unit vector square to `direction`.
glm::vec3 AnyPerpendicular(const glm::vec3& direction)
{
    const glm::vec3 other = std::abs(direction.y) < 0.9f ? kUp : kForward;
    return glm::normalize(glm::cross(direction, other));
}

// The shortest turn taking direction `from` to `to`.
glm::quat TurnBetween(const glm::vec3& from, const glm::vec3& to)
{
    const glm::vec3 a = SafeNormalize(from, kForward);
    const glm::vec3 b = SafeNormalize(to, kForward);
    const float cosine = glm::dot(a, b);
    if (cosine < -0.9999f)
    {
        return glm::angleAxis(glm::pi<float>(), AnyPerpendicular(a));
    }
    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

// The orthonormal frame with its X along `primary` and its Y towards `secondary`.
glm::mat3 FrameOf(const glm::vec3& primary, const glm::vec3& secondary)
{
    const glm::vec3 x = SafeNormalize(primary, kForward);
    glm::vec3 z = glm::cross(x, secondary);
    z = glm::length(z) > 1e-6f ? glm::normalize(z) : AnyPerpendicular(x);
    const glm::vec3 y = glm::cross(z, x);
    return glm::mat3(x, y, z);
}

// The turn taking `fromPrimary` to `toPrimary` exactly and `fromSecondary` as near to `toSecondary`
// as that allows.
glm::quat TurnFrames(const glm::vec3& fromPrimary, const glm::vec3& fromSecondary, const glm::vec3& toPrimary, const glm::vec3& toSecondary)
{
    const glm::mat3 from = FrameOf(fromPrimary, fromSecondary);
    const glm::mat3 to = FrameOf(toPrimary, toSecondary);
    return glm::normalize(glm::quat_cast(to * glm::transpose(from)));
}

// The part of `turn` about `axis` (unit).
glm::quat TwistAbout(const glm::quat& turn, const glm::vec3& axis)
{
    const glm::vec3 vector(turn.x, turn.y, turn.z);
    const glm::vec3 projected = axis * glm::dot(vector, axis);
    const glm::quat twist(turn.w, projected.x, projected.y, projected.z);
    const float length = glm::length(twist);
    return length > 1e-6f ? twist / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

glm::quat Fraction(const glm::quat& turn, float share)
{
    return glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), turn, share);
}

// Where the middle joint of a two-bone chain (lengths upper, lower) goes for its end to reach
// `target` from `root`, bent towards `pole`; `reached` is where the end gets to (the target, or as
// near as the chain reaches).
glm::vec3 SolveMiddleJoint(const glm::vec3& root, const glm::vec3& target, float upper, float lower, const glm::vec3& pole, glm::vec3& reached)
{
    const glm::vec3 offset = target - root;
    const float distance = glm::length(offset);
    const glm::vec3 direction = distance > 1e-6f ? offset / distance : kForward;
    const float reach = std::clamp(distance, std::abs(upper - lower) + 1e-4f, (upper + lower) * 0.9995f);
    reached = root + direction * reach;
    const float cosine = std::clamp((upper * upper + reach * reach - lower * lower) / (2.0f * upper * reach), -1.0f, 1.0f);
    const glm::vec3 bend = SafeNormalize(pole - direction * glm::dot(pole, direction), AnyPerpendicular(direction));
    return root + direction * (upper * cosine) + bend * (upper * std::sqrt(1.0f - cosine * cosine));
}

// The local poses and the model-space transforms they give, kept in step.
class Posing
{
  public:
    Posing(const ModelSkeleton& skeleton, std::vector<ModelNodePose>& poses) : m_skeleton(skeleton), m_poses(poses)
    {
        Update();
    }

    void Update()
    {
        ComputeNodeWorldMatrices(m_skeleton, m_poses, m_world);
    }

    glm::vec3 Position(int32_t node) const
    {
        return glm::vec3(m_world[static_cast<size_t>(node)][3]);
    }

    glm::quat Rotation(int32_t node) const
    {
        return RotationOf(m_world[static_cast<size_t>(node)]);
    }

    // Turns the node about itself by `turn`, in the model's space. Update before reading its children.
    void Turn(int32_t node, const glm::quat& turn)
    {
        const int32_t parent = m_skeleton.nodes[static_cast<size_t>(node)].parent;
        const glm::quat parentRotation = parent >= 0 ? Rotation(parent) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        ModelNodePose& pose = m_poses[static_cast<size_t>(node)];
        pose.rotation = glm::normalize(glm::conjugate(parentRotation) * (turn * Rotation(node)));
        pose.posed = true;
    }

    // Moves the node by `offset`, in the model's space.
    void Move(int32_t node, const glm::vec3& offset)
    {
        const int32_t parent = m_skeleton.nodes[static_cast<size_t>(node)].parent;
        const glm::mat3 parentLinear = parent >= 0 ? glm::mat3(m_world[static_cast<size_t>(parent)]) : glm::mat3(1.0f);
        ModelNodePose& pose = m_poses[static_cast<size_t>(node)];
        pose.translation += glm::inverse(parentLinear) * offset;
        pose.posed = true;
    }

    ModelNodePose& Pose(int32_t node)
    {
        return m_poses[static_cast<size_t>(node)];
    }

    const std::vector<glm::mat4>& World() const
    {
        return m_world;
    }

    static glm::quat RotationOf(const glm::mat4& matrix)
    {
        const glm::mat3 linear(
            SafeNormalize(glm::vec3(matrix[0]), kLeft),
            SafeNormalize(glm::vec3(matrix[1]), kUp),
            SafeNormalize(glm::vec3(matrix[2]), kForward));
        return glm::normalize(glm::quat_cast(linear));
    }

  private:
    const ModelSkeleton& m_skeleton;
    std::vector<ModelNodePose>& m_poses;
    std::vector<glm::mat4> m_world;
};

// The rest pose's hinges and faces, in their joints' own frames: a rig is in a T-pose (arms out, palms
// down, feet flat), its elbows bend the forearm forward and its knees the shin back.
struct RestFrames
{
    std::array<glm::vec3, 2> elbowHinge{};
    std::array<glm::vec3, 2> kneeHinge{};
    std::array<glm::vec3, 2> palm{};
    std::array<glm::vec3, 2> footSide{};
    // How far the line from the ankle to the ball of the foot points below level (radians, negative).
    std::array<float, 2> footPitch{};
};

RestFrames ComputeRestFrames(const ModelSkeleton& skeleton, const DriverRig& rig)
{
    std::vector<ModelNodePose> poses;
    RestNodePoses(skeleton, poses);
    std::vector<glm::mat4> world;
    ComputeNodeWorldMatrices(skeleton, poses, world);
    const auto position = [&](int32_t node)
    {
        return glm::vec3(world[static_cast<size_t>(node)][3]);
    };
    const auto toLocal = [&](int32_t node, const glm::vec3& direction)
    {
        return glm::conjugate(Posing::RotationOf(world[static_cast<size_t>(node)])) * direction;
    };
    RestFrames frames;
    for (size_t side = 0; side < 2; ++side)
    {
        const glm::vec3 upperArm = SafeNormalize(position(rig.elbow[side]) - position(rig.shoulder[side]), kLeft);
        frames.elbowHinge[side] = toLocal(rig.shoulder[side], SafeNormalize(glm::cross(upperArm, kForward), kUp));
        const glm::vec3 thigh = SafeNormalize(position(rig.knee[side]) - position(rig.hip[side]), -kUp);
        frames.kneeHinge[side] = toLocal(rig.hip[side], SafeNormalize(glm::cross(thigh, -kForward), kLeft));
        frames.palm[side] = toLocal(rig.wrist[side], -kUp);
        frames.footSide[side] = toLocal(rig.ankle[side], kLeft);
        const glm::vec3 foot = SafeNormalize(position(rig.toes[side]) - position(rig.ankle[side]), kForward);
        frames.footPitch[side] = std::asin(std::clamp(foot.y, -1.0f, 1.0f));
    }
    return frames;
}

int32_t Find(const ModelSkeleton& skeleton, const std::string& name)
{
    return skeleton.FindNode(name);
}
}

std::optional<DriverRig> FindDriverRig(const ModelSkeleton& skeleton)
{
    DriverRig rig;
    rig.root = Find(skeleton, "Root_M");
    rig.spine = Find(skeleton, "Spine1_M");
    rig.chest = Find(skeleton, "Chest_M");
    rig.neck = Find(skeleton, "Neck_M");
    rig.head = Find(skeleton, "Head_M");
    static constexpr std::array<const char*, 2> kSides{"_L", "_R"};
    static constexpr std::array<const char*, 5> kFingerNames{"ThumbFinger", "IndexFinger", "MiddleFinger", "RingFinger", "PinkyFinger"};
    bool valid = rig.root >= 0 && rig.spine >= 0 && rig.chest >= 0 && rig.neck >= 0 && rig.head >= 0;
    for (size_t side = 0; side < 2; ++side)
    {
        const std::string suffix = kSides[side];
        rig.hip[side] = Find(skeleton, "Hip" + suffix);
        rig.knee[side] = Find(skeleton, "Knee" + suffix);
        rig.ankle[side] = Find(skeleton, "Ankle" + suffix);
        rig.toes[side] = Find(skeleton, "Toes" + suffix);
        rig.scapula[side] = Find(skeleton, "Scapula" + suffix);
        rig.shoulder[side] = Find(skeleton, "Shoulder" + suffix);
        rig.elbow[side] = Find(skeleton, "Elbow" + suffix);
        rig.wrist[side] = Find(skeleton, "Wrist" + suffix);
        rig.forearmTwist[side] = {Find(skeleton, "ElbowPart1" + suffix), Find(skeleton, "ElbowPart2" + suffix)};
        rig.handEnd[side] = Find(skeleton, "MiddleFinger1" + suffix);
        rig.eye[side] = Find(skeleton, "Eye" + suffix);
        for (size_t finger = 0; finger < kFingerNames.size(); ++finger)
        {
            for (size_t joint = 0; joint < 3; ++joint)
            {
                rig.fingers[side][finger][joint] = Find(skeleton, std::string(kFingerNames[finger]) + std::to_string(joint + 1) + suffix);
            }
        }
        valid = valid && rig.hip[side] >= 0 && rig.knee[side] >= 0 && rig.ankle[side] >= 0 && rig.toes[side] >= 0 &&
                rig.shoulder[side] >= 0 && rig.elbow[side] >= 0 && rig.wrist[side] >= 0 && rig.handEnd[side] >= 0;
    }
    if (!valid)
    {
        return std::nullopt;
    }
    return rig;
}

void PoseDriver(const ModelSkeleton& skeleton, const DriverRig& rig, const DriverPoseInput& input, std::vector<ModelNodePose>& poses,
                DriverPoseResult* result)
{
    const RestFrames rest = ComputeRestFrames(skeleton, rig);
    RestNodePoses(skeleton, poses);
    Posing posing(skeleton, poses);

    // The back leans back with the pelvis; the thighs are aimed afterwards.
    const float recline = glm::radians(input.reclineDegrees);
    posing.Turn(rig.root, glm::angleAxis(-recline, kLeft));
    posing.Update();
    const glm::vec3 hipsNow = (posing.Position(rig.hip[0]) + posing.Position(rig.hip[1])) * 0.5f;
    posing.Move(rig.root, input.hips - hipsNow);
    posing.Update();

    // The back bends forward from the seat, half at the waist and half at the chest; the neck and head
    // come back towards upright and turn towards the corner.
    const float lean = glm::radians(input.leanDegrees);
    posing.Turn(rig.spine, glm::angleAxis(lean * 0.5f, kLeft));
    posing.Update();
    posing.Turn(rig.chest, glm::angleAxis(lean * 0.5f, kLeft));
    posing.Update();
    const float backFromUpright = recline - lean;
    const float headYaw = glm::radians(input.headYawDegrees);
    posing.Turn(rig.neck, glm::angleAxis(headYaw * 0.4f, kUp) * glm::angleAxis(backFromUpright * kNeckUprightShare, kLeft));
    posing.Update();
    posing.Turn(rig.head, glm::angleAxis(headYaw * 0.6f, kUp) * glm::angleAxis(backFromUpright * kHeadUprightShare, kLeft));
    posing.Update();

    // Legs: the knees up, the ankles on the pedals, the feet up the pedals' slope.
    // Where the thighs point once seated, which a skirt's front lies along.
    std::array<glm::vec3, 2> thighs{};
    for (size_t side = 0; side < 2; ++side)
    {
        const float outward = side == 0 ? 1.0f : -1.0f;
        const glm::vec3 hip = posing.Position(rig.hip[side]);
        const glm::vec3 knee = posing.Position(rig.knee[side]);
        const glm::vec3 ankle = posing.Position(rig.ankle[side]);
        const float thighLength = glm::distance(hip, knee);
        const float shinLength = glm::distance(knee, ankle);
        // Up, and out enough for the knees to pass outside the wheel's rim.
        const glm::vec3 pole = glm::normalize(kUp + kForward * 0.6f + kLeft * (outward * 0.35f));
        glm::vec3 reached;
        const glm::vec3 newKnee = SolveMiddleJoint(hip, input.ankles[side], thighLength, shinLength, pole, reached);

        const glm::vec3 thigh = glm::normalize(knee - hip);
        const glm::vec3 newThigh = glm::normalize(newKnee - hip);
        const glm::vec3 newShin = glm::normalize(reached - newKnee);
        const glm::vec3 hinge = posing.Rotation(rig.hip[side]) * rest.kneeHinge[side];
        const glm::quat turn = TurnFrames(thigh, hinge, newThigh, glm::cross(newThigh, newShin));
        posing.Turn(rig.hip[side], turn);
        posing.Update();
        thighs[side] = newThigh;

        const glm::vec3 shin = posing.Position(rig.ankle[side]) - posing.Position(rig.knee[side]);
        posing.Turn(rig.knee[side], TurnBetween(shin, newShin));
        posing.Update();

        const glm::vec3 foot = posing.Position(rig.toes[side]) - posing.Position(rig.ankle[side]);
        const glm::vec3 footSide = posing.Rotation(rig.ankle[side]) * rest.footSide[side];
        // The rest pose's foot, flat on the floor, raised up the pedal and pointing straight ahead.
        const float pitch = rest.footPitch[side] + glm::radians(kFootRaiseDegrees);
        const glm::vec3 newFoot = kForward * std::cos(pitch) + kUp * std::sin(pitch);
        posing.Turn(rig.ankle[side], TurnFrames(foot, footSide, newFoot, kLeft));
        posing.Update();
    }

    // A skirt hanging from the pelvis in chains of joints (Skirt_*): its front lies along the thighs
    // onto the lap, its sides partly, its back stays on the seat. Each chain turns at its first joint
    // so that its end lies that way.
    std::vector<int32_t> firstChild(skeleton.nodes.size(), -1);
    for (size_t index = skeleton.nodes.size(); index-- > 0;)
    {
        const int32_t parent = skeleton.nodes[index].parent;
        if (parent >= 0)
        {
            firstChild[static_cast<size_t>(parent)] = static_cast<int32_t>(index);
        }
    }
    const glm::vec3 pelvis = posing.Position(rig.root);
    for (size_t child = 0; child < skeleton.nodes.size(); ++child)
    {
        const ModelSkeletonNode& node = skeleton.nodes[child];
        if (node.parent != rig.root || node.name.rfind("Skirt", 0) != 0)
        {
            continue;
        }
        int32_t end = static_cast<int32_t>(child);
        while (firstChild[static_cast<size_t>(end)] >= 0)
        {
            end = firstChild[static_cast<size_t>(end)];
        }
        const glm::vec3 start = posing.Position(static_cast<int32_t>(child));
        const glm::vec3 hanging = posing.Position(end) - start;
        if (glm::length(hanging) < 1e-4f)
        {
            continue;
        }
        const glm::vec3 offset = start - pelvis;
        const glm::vec3 across = SafeNormalize(glm::vec3(offset.x, 0.0f, offset.z), kForward);
        const float front = glm::smoothstep(-0.6f, 0.4f, glm::dot(across, kForward));
        const size_t side = offset.x >= 0.0f ? 0 : 1;
        const glm::vec3 onLap = glm::mix(glm::normalize(hanging), thighs[side], front);
        posing.Turn(static_cast<int32_t>(child), TurnBetween(hanging, onLap));
    }
    posing.Update();

    // Arms: the hands on the rim, the elbows down and a little out.
    const glm::vec3 wheelAxis = SafeNormalize(input.wheelAxis, kForward);
    const glm::vec3 wheelUp = SafeNormalize(kUp - wheelAxis * glm::dot(kUp, wheelAxis), kUp);
    const glm::vec3 wheelLeft = glm::normalize(glm::cross(wheelUp, wheelAxis));
    const float gripLift = glm::radians(kGripAboveLevelDegrees);
    const float maxHandTurn = glm::radians(kMaxHandTurnDegrees);
    for (size_t side = 0; side < 2; ++side)
    {
        const float outward = side == 0 ? 1.0f : -1.0f;
        const glm::vec3 restRadial = glm::normalize(wheelLeft * (outward * std::cos(gripLift)) + wheelUp * std::sin(gripLift));
        const float handTurn = std::clamp(input.wheelTurn, -maxHandTurn, maxHandTurn);
        const glm::vec3 radial = glm::angleAxis(handTurn, wheelAxis) * restRadial;
        const glm::vec3 grip = input.wheelCenter + radial * input.wheelRadius;
        const glm::vec3 wristTarget = grip - wheelAxis * kWristBehindRim + radial * kWristOutsideRim;

        // Nearly straight arms reach with the shoulder blades too.
        if (rig.scapula[side] >= 0)
        {
            const glm::vec3 blade = posing.Position(rig.scapula[side]);
            const glm::vec3 shoulder = posing.Position(rig.shoulder[side]);
            const float armLength = glm::distance(shoulder, posing.Position(rig.elbow[side])) +
                                    glm::distance(posing.Position(rig.elbow[side]), posing.Position(rig.wrist[side]));
            const float stretch = glm::smoothstep(0.8f, 1.05f, glm::distance(shoulder, wristTarget) / armLength);
            const glm::quat towards = TurnBetween(shoulder - blade, wristTarget - blade);
            const float angle = glm::angle(towards);
            const float share = angle > 1e-5f ? std::min(1.0f, glm::radians(kMaxShoulderReachDegrees) / angle) * stretch : 0.0f;
            posing.Turn(rig.scapula[side], Fraction(towards, share));
            posing.Update();
        }

        const glm::vec3 shoulder = posing.Position(rig.shoulder[side]);
        const glm::vec3 elbow = posing.Position(rig.elbow[side]);
        const glm::vec3 wrist = posing.Position(rig.wrist[side]);
        const float upperLength = glm::distance(shoulder, elbow);
        const float lowerLength = glm::distance(elbow, wrist);
        const glm::vec3 pole = glm::normalize(-kUp + kLeft * (outward * 0.5f) - kForward * 0.2f);
        glm::vec3 reached;
        const glm::vec3 newElbow = SolveMiddleJoint(shoulder, wristTarget, upperLength, lowerLength, pole, reached);
        if (result != nullptr)
        {
            result->armStretch[side] = glm::distance(shoulder, wristTarget) / (upperLength + lowerLength);
        }

        const glm::vec3 upper = glm::normalize(elbow - shoulder);
        const glm::vec3 newUpper = glm::normalize(newElbow - shoulder);
        const glm::vec3 newLower = glm::normalize(reached - newElbow);
        const glm::vec3 hinge = posing.Rotation(rig.shoulder[side]) * rest.elbowHinge[side];
        posing.Turn(rig.shoulder[side], TurnFrames(upper, hinge, newUpper, glm::cross(newUpper, newLower)));
        posing.Update();
        const glm::vec3 lower = posing.Position(rig.wrist[side]) - posing.Position(rig.elbow[side]);
        posing.Turn(rig.elbow[side], TurnBetween(lower, newLower));
        posing.Update();

        // The hand reaches over the rim, its palm towards the wheel's centre; the forearm's twist
        // joints take a share of its turn about the forearm.
        const glm::vec3 handDirection = glm::normalize(wheelAxis - radial * 0.25f);
        const glm::vec3 palmFacing = -radial;
        const glm::vec3 hand = posing.Position(rig.handEnd[side]) - posing.Position(rig.wrist[side]);
        const glm::vec3 palm = posing.Rotation(rig.wrist[side]) * rest.palm[side];
        const glm::quat handTurnFrames = TurnFrames(hand, palm, handDirection, palmFacing);
        const glm::quat twist = TwistAbout(handTurnFrames, newLower);
        for (size_t part = 0; part < 2; ++part)
        {
            if (rig.forearmTwist[side][part] >= 0)
            {
                posing.Turn(rig.forearmTwist[side][part], Fraction(twist, (static_cast<float>(part) + 1.0f) / 3.0f));
            }
        }
        posing.Turn(rig.wrist[side], handTurnFrames);
        posing.Update();

        // The fingers close round the rim: each joint turns about the hand's width, towards the palm.
        const glm::vec3 palmNow = posing.Rotation(rig.wrist[side]) * rest.palm[side];
        const glm::vec3 handNow = glm::normalize(posing.Position(rig.handEnd[side]) - posing.Position(rig.wrist[side]));
        const glm::vec3 curlAxis = SafeNormalize(glm::cross(handNow, palmNow), kLeft);
        for (size_t finger = 0; finger < rig.fingers[side].size(); ++finger)
        {
            const std::array<float, 3>& curl = finger == 0 ? kThumbCurlDegrees : kFingerCurlDegrees;
            for (size_t joint = 0; joint < 3; ++joint)
            {
                const int32_t node = rig.fingers[side][finger][joint];
                if (node < 0)
                {
                    continue;
                }
                // About the same axis for every joint of the hand: a parent's turn leaves its
                // children's axis where it is, so each joint's own turn can be set before any update.
                const glm::vec3 localAxis = glm::conjugate(posing.Rotation(node)) * curlAxis;
                ModelNodePose& pose = posing.Pose(node);
                pose.rotation = glm::normalize(pose.rotation * glm::angleAxis(glm::radians(curl[joint]), localAxis));
                pose.posed = true;
            }
        }
        posing.Update();
    }

    if (result != nullptr)
    {
        if (rig.eye[0] >= 0 && rig.eye[1] >= 0)
        {
            result->eyes = (posing.Position(rig.eye[0]) + posing.Position(rig.eye[1])) * 0.5f;
        }
        else
        {
            result->eyes = posing.Position(rig.head) + kUp * 0.1f;
        }
    }
    if (input.hideHead)
    {
        ModelNodePose& head = posing.Pose(rig.head);
        head.scale = glm::vec3(kHiddenHeadScale);
        head.posed = true;
    }
}

void EvaluateDriverPalette(const ModelSkeleton& skeleton, const DriverRig& rig, const DriverPoseInput& input, std::vector<glm::mat4>& palette,
                           DriverPoseResult* result)
{
    std::vector<ModelNodePose> poses;
    PoseDriver(skeleton, rig, input, poses, result);
    std::vector<glm::mat4> world;
    ComputeNodeWorldMatrices(skeleton, poses, world);
    PaletteFromNodeWorldMatrices(skeleton, world, palette);
}
}
