#include "vehicle_driver_service.h"

#include <engine/asset/model_cache.h>
#include <engine/core/log/log.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/vehicle_gear_shift.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace me
{

namespace
{
// The driver's eyes from the steering wheel's centre when the car's data does not place them, as the
// R34's DRIVEREYES sit from its STEER_HR (VehicleDriveService does the same for the cockpit camera).
constexpr float kEyesBehindSteeringWheel = 0.59f;
constexpr float kEyesOverSteeringWheel = 0.29f;
// The seat is looked for this far below the eyes, a little ahead of them; its back at two heights over
// the cushion, from this far ahead of the eyes.
constexpr float kCushionRayStartBelowEyes = 0.2f;
constexpr float kCushionRayAhead = 0.05f;
constexpr float kCushionFallbackBelowEyes = 0.72f;
constexpr float kBackRayAhead = 0.25f;
constexpr float kBackLowerHeight = 0.15f;
constexpr float kBackUpperHeight = 0.40f;
constexpr float kBackFallbackBehindEyes = 0.25f;
constexpr float kMinReclineDegrees = 5.0f;
constexpr float kMaxReclineDegrees = 35.0f;
constexpr float kFallbackReclineDegrees = 15.0f;
// The hip joints sit this far over the cushion and ahead of the back (SAE's H-point manikin).
constexpr float kHipsAboveCushion = 0.09f;
constexpr float kHipsBeforeBack = 0.13f;
// The floor is looked for this far ahead of the hips, clear of the seat's front edge, and the toe
// board forward from there, this far over the floor.
constexpr float kFloorRayAhead = 0.65f;
constexpr float kToeBoardRayHeight = 0.12f;
constexpr float kFloorFallbackBelowCushion = 0.2f;
constexpr float kToeBoardFallbackAhead = 0.95f;
// The pedals are looked for across the footwell this far each side of the steering wheel's centre, in
// steps this wide, this high over the floor, out to this far ahead of where the floor was found: what
// stands between these two distances before the toe board, between these two widths, is a pedal (the
// door's trim, slanting across the scan's end, shows as a sliver). From the
// driver's right: the accelerator, the brake, the clutch and the foot rest.
constexpr float kPedalScanHalfWidth = 0.32f;
constexpr float kPedalScanStep = 0.01f;
constexpr float kPedalScanHeight = 0.12f;
constexpr float kPedalScanLength = 0.6f;
constexpr float kPedalStandOff = 0.04f;
constexpr float kMaxPedalStandOff = 0.2f;
constexpr float kMinPedalWidth = 0.03f;
constexpr float kMaxPedalWidth = 0.14f;
// Where the pedals are when none are found: across from the steering wheel's centre (to its left),
// this far before the toe board.
constexpr float kFallbackThrottleAcross = -0.18f;
constexpr float kFallbackBrakeAcross = -0.08f;
constexpr float kFallbackClutchAcross = 0.03f;
constexpr float kFallbackFootRestAcross = 0.16f;
constexpr float kFallbackPedalBeforeToeBoard = 0.08f;
// The ball of the foot this far behind a pedal's face (the sole), the foot raised this much from flat
// on a pedal at rest. A press tips the foot forward and moves the ankle forward: the accelerator
// hinges forward, the brake pushes, the clutch goes a long way.
constexpr float kSoleThickness = 0.02f;
constexpr float kFootRaiseDegrees = 50.0f;
struct PedalTravel
{
    float tipDegrees;
    float forward;
};
constexpr PedalTravel kThrottleTravel{15.0f, 0.02f};
constexpr PedalTravel kBrakeTravel{8.0f, 0.04f};
constexpr PedalTravel kClutchTravel{5.0f, 0.09f};
// A foot moves between pedals in this long, lifted this much at the middle; it stays on the clutch this
// long after the clutch closed, and presses follow the pedals over about this long.
constexpr float kFootMoveSeconds = 0.15f;
constexpr float kFootLift = 0.04f;
constexpr float kClutchHoldSeconds = 0.4f;
constexpr float kPressSeconds = 0.05f;
// Pedal inputs below this are none.
constexpr float kPedalThreshold = 0.03f;
// A hand on the gear lever holds the knob from above, a little ahead, its fingers pointing forward and
// down.
constexpr float kKnobGripAbove = 0.015f;
// Legs straightened to at most this share of their length.
constexpr float kMaxLegStretch = 0.95f;
// The hands hold the rim at its middle: the rim's outer edge less half its thickness.
constexpr float kRimHalfThickness = 0.012f;
// For the hands to reach the wheel, the back bends forward off the seat's in these steps up to this
// far, then the hips slide forward too, until neither wrist needs more than this share of its arm's
// length: a short driver leans in before the knees come up under the wheel.
constexpr float kMaxArmStretch = 0.98f;
constexpr float kLeanStepDegrees = 2.0f;
constexpr float kMaxLeanDegrees = 20.0f;
constexpr float kSlideStep = 0.01f;
constexpr float kMaxSlide = 0.3f;
// The head turns this share of the steering wheel's turn into the corner, up to this far.
constexpr float kHeadYawShare = 0.1f;
constexpr float kMaxHeadYawDegrees = 20.0f;

// Rays through the car's own meshes, in vehicle space: everything but its wheels, its steering wheel,
// glass and the top of water.
class CarRays
{
  public:
    CarRays(const LoadedModelData& car, const glm::quat& vehicleToModel) : m_car(car), m_vehicleToModel(vehicleToModel)
    {
    }

    // How far along `direction` (unit) from `origin` the first surface is, within maxDistance.
    std::optional<float> Cast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const
    {
        const glm::vec3 o = m_vehicleToModel * origin;
        const glm::vec3 d = m_vehicleToModel * direction;
        float nearest = maxDistance;
        bool hit = false;
        for (const ModelSubmeshData& submesh : m_car.submeshes)
        {
            // Assetto Corsa's seat belt (CINTURE_ON) hangs in front of the seat, round its own driver.
            if (submesh.wheelPart != ModelWheelPart::None || submesh.steeringWheel || submesh.water || submesh.mesh.IsSkinned() ||
                !submesh.mesh.IsValid() || IsBlended(submesh) || submesh.name.find("CINTURE") != std::string::npos)
            {
                continue;
            }
            // Past the submesh's bounding sphere, or behind the ray: none of its triangles.
            const glm::vec3 toCenter = submesh.boundsCenter - o;
            const float along = glm::dot(toCenter, d);
            const float squaredMiss = glm::dot(toCenter, toCenter) - along * along;
            if (squaredMiss > submesh.boundsRadius * submesh.boundsRadius || along + submesh.boundsRadius < 0.0f ||
                along - submesh.boundsRadius > nearest)
            {
                continue;
            }
            const std::vector<Vertex>& vertices = submesh.mesh.vertices;
            const std::vector<uint32_t>& indices = submesh.mesh.indices;
            for (size_t index = 0; index + 2 < indices.size(); index += 3)
            {
                const glm::vec3 a = Position(vertices[indices[index]]);
                const glm::vec3 b = Position(vertices[indices[index + 1]]);
                const glm::vec3 c = Position(vertices[indices[index + 2]]);
                // Möller-Trumbore, either face.
                const glm::vec3 edge1 = b - a;
                const glm::vec3 edge2 = c - a;
                const glm::vec3 p = glm::cross(d, edge2);
                const float determinant = glm::dot(edge1, p);
                if (std::abs(determinant) < 1e-12f)
                {
                    continue;
                }
                const float inverse = 1.0f / determinant;
                const glm::vec3 s = o - a;
                const float u = glm::dot(s, p) * inverse;
                if (u < 0.0f || u > 1.0f)
                {
                    continue;
                }
                const glm::vec3 q = glm::cross(s, edge1);
                const float v = glm::dot(d, q) * inverse;
                if (v < 0.0f || u + v > 1.0f)
                {
                    continue;
                }
                const float t = glm::dot(edge2, q) * inverse;
                if (t > 0.0f && t < nearest)
                {
                    nearest = t;
                    hit = true;
                }
            }
        }
        return hit ? std::optional<float>(nearest) : std::nullopt;
    }

  private:
    static glm::vec3 Position(const Vertex& vertex)
    {
        return glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]);
    }

    bool IsBlended(const ModelSubmeshData& submesh) const
    {
        return submesh.materialIndex < m_car.materials.size() && m_car.materials[submesh.materialIndex].alphaMode == MaterialAlphaMode::Blend;
    }

    const LoadedModelData& m_car;
    glm::quat m_vehicleToModel;
};

// The rim's radius about the column: the steering wheel's farthest vertex from it.
float SteeringWheelRadius(const LoadedModelData& car, const ModelSteeringWheel& wheel)
{
    float radius = 0.0f;
    const glm::vec3 axis = glm::normalize(wheel.axis);
    for (const ModelSubmeshData& submesh : car.submeshes)
    {
        if (!submesh.steeringWheel)
        {
            continue;
        }
        for (const Vertex& vertex : submesh.mesh.vertices)
        {
            const glm::vec3 offset = glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]) - wheel.center;
            radius = std::max(radius, glm::length(offset - axis * glm::dot(offset, axis)));
        }
    }
    return radius;
}

glm::vec3 RestPosition(const std::vector<glm::mat4>& world, int32_t node)
{
    return glm::vec3(world[static_cast<size_t>(node)][3]);
}

bool NearlyEqual(const glm::mat4& a, const glm::mat4& b)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (std::abs(a[column][row] - b[column][row]) > 1e-5f)
            {
                return false;
            }
        }
    }
    return true;
}

entt::entity FindEntityById(const IEditorWorld& world, VehicleDriverState& drivers, const std::string& id)
{
    if (const auto found = drivers.entitiesById.find(id); found != drivers.entitiesById.end())
    {
        if (world.IsValidEntity(found->second) && world.Registry().all_of<SceneEntityIdComponent>(found->second) &&
            world.GetEntityUuid(found->second) == id)
        {
            return found->second;
        }
        drivers.entitiesById.erase(found);
    }
    for (const auto [entity, entityId] : world.Registry().view<const SceneEntityIdComponent>().each())
    {
        if (entityId.value == id)
        {
            drivers.entitiesById[id] = entity;
            return entity;
        }
    }
    return entt::null;
}
}

std::optional<VehicleDriverSeat> FitDriverSeat(
    const LoadedModelData& car,
    const glm::quat& vehicleToModel,
    const ModelSkeleton& skeleton,
    const DriverRig& rig)
{
    if (!car.steeringWheel.has_value())
    {
        return std::nullopt;
    }
    const glm::quat modelToVehicle = glm::conjugate(vehicleToModel);
    VehicleDriverSeat seat;
    seat.wheelCenter = modelToVehicle * car.steeringWheel->center;
    seat.wheelAxis = glm::normalize(modelToVehicle * car.steeringWheel->axis);
    if (seat.wheelAxis.z < 0.0f)
    {
        seat.wheelAxis = -seat.wheelAxis;
        seat.wheelTurnSign = -1.0f;
    }
    seat.wheelRadius = std::max(SteeringWheelRadius(car, *car.steeringWheel) - kRimHalfThickness, 0.1f);

    glm::vec3 eyes = seat.wheelCenter + glm::vec3(0.0f, kEyesOverSteeringWheel, -kEyesBehindSteeringWheel);
    if (car.carSpec.has_value() && car.carSpec->cockpitCamera.has_value())
    {
        eyes = modelToVehicle * car.carSpec->cockpitCamera->position;
    }
    seat.carEyes = eyes;

    // The seat in line with the steering wheel.
    const CarRays rays(car, vehicleToModel);
    const float x = seat.wheelCenter.x;
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    const glm::vec3 forward(0.0f, 0.0f, 1.0f);

    const glm::vec3 cushionRay(x, eyes.y - kCushionRayStartBelowEyes, eyes.z + kCushionRayAhead);
    const std::optional<float> cushionHit = rays.Cast(cushionRay, down, 1.0f);
    const float cushion = cushionHit.has_value() ? cushionRay.y - *cushionHit : eyes.y - kCushionFallbackBelowEyes;

    const std::optional<float> backLowerHit = rays.Cast(glm::vec3(x, cushion + kBackLowerHeight, eyes.z + kBackRayAhead), -forward, 1.0f);
    const std::optional<float> backUpperHit = rays.Cast(glm::vec3(x, cushion + kBackUpperHeight, eyes.z + kBackRayAhead), -forward, 1.0f);
    float back = eyes.z - kBackFallbackBehindEyes;
    seat.reclineDegrees = kFallbackReclineDegrees;
    if (backLowerHit.has_value())
    {
        back = eyes.z + kBackRayAhead - *backLowerHit;
        if (backUpperHit.has_value())
        {
            const float lean = *backUpperHit - *backLowerHit;
            seat.reclineDegrees = std::clamp(
                glm::degrees(std::atan2(lean, kBackUpperHeight - kBackLowerHeight)), kMinReclineDegrees, kMaxReclineDegrees);
        }
    }
    seat.hips = glm::vec3(x, cushion + kHipsAboveCushion, back + kHipsBeforeBack);

    const glm::vec3 floorRay(x, cushion, seat.hips.z + kFloorRayAhead);
    const std::optional<float> floorHit = rays.Cast(floorRay, down, 1.0f);
    const float floor = floorHit.has_value() ? floorRay.y - *floorHit : cushion - kFloorFallbackBelowCushion;
    // Across the footwell at the pedals' height: most rays meet the toe board, the pedals stand before
    // it (a ray through a gap may reach the engine bay; the door's trim is nearer at one side).
    const float pedalHeight = floor + kPedalScanHeight;
    std::vector<std::pair<float, float>> scan;
    for (float across = -kPedalScanHalfWidth; across <= kPedalScanHalfWidth + 1e-4f; across += kPedalScanStep)
    {
        const glm::vec3 origin(x + across, pedalHeight, floorRay.z);
        if (const std::optional<float> hit = rays.Cast(origin, forward, kPedalScanLength))
        {
            scan.emplace_back(origin.x, origin.z + *hit);
        }
    }
    const bool toeBoardFound = !scan.empty();
    float toeBoard = seat.hips.z + kToeBoardFallbackAhead;
    if (toeBoardFound)
    {
        std::vector<float> depths;
        for (const auto& [across, depth] : scan)
        {
            depths.push_back(depth);
        }
        std::sort(depths.begin(), depths.end());
        toeBoard = depths[(depths.size() * 3) / 4];
    }
    std::vector<glm::vec3> pedals;
    for (size_t index = 0; index < scan.size();)
    {
        size_t end = index;
        float nearest = 1e9f;
        while (end < scan.size() && scan[end].second < toeBoard - kPedalStandOff && scan[end].second > toeBoard - kMaxPedalStandOff &&
               (end == index || scan[end].first - scan[end - 1].first < kPedalScanStep * 1.5f))
        {
            nearest = std::min(nearest, scan[end].second);
            ++end;
        }
        if (end == index)
        {
            ++index;
            continue;
        }
        const float width = scan[end - 1].first - scan[index].first + kPedalScanStep;
        if (width >= kMinPedalWidth - 1e-4f && width <= kMaxPedalWidth)
        {
            pedals.emplace_back((scan[index].first + scan[end - 1].first) * 0.5f, pedalHeight, nearest);
        }
        index = end;
    }
    // From the driver's right (vehicle space's -X) to the left.
    std::sort(pedals.begin(), pedals.end(), [](const glm::vec3& a, const glm::vec3& b)
              {
                  return a.x < b.x;
              });
    const auto fallback = [&](float across)
    {
        return glm::vec3(x + across, pedalHeight, toeBoard - kFallbackPedalBeforeToeBoard);
    };
    seat.pedalsFound = pedals.size() >= 2;
    seat.throttle = seat.pedalsFound ? pedals[0] : fallback(kFallbackThrottleAcross);
    seat.brake = seat.pedalsFound ? pedals[1] : fallback(kFallbackBrakeAcross);
    seat.clutch = pedals.size() >= 3 ? pedals[2] : seat.brake + (seat.brake - seat.throttle);
    seat.footRest = pedals.size() >= 4 ? pedals[3] : seat.clutch + glm::vec3(kFallbackFootRestAcross - kFallbackClutchAcross, 0.0f, 0.0f);

    // The legs and feet, from the rest pose.
    std::vector<ModelNodePose> rest;
    RestNodePoses(skeleton, rest);
    std::vector<glm::mat4> restWorld;
    ComputeNodeWorldMatrices(skeleton, rest, restWorld);
    seat.legLength = glm::distance(RestPosition(restWorld, rig.hip[0]), RestPosition(restWorld, rig.knee[0])) +
                     glm::distance(RestPosition(restWorld, rig.knee[0]), RestPosition(restWorld, rig.ankle[0]));
    seat.hipHalfWidth = glm::distance(RestPosition(restWorld, rig.hip[0]), RestPosition(restWorld, rig.hip[1])) * 0.5f;
    const glm::vec3 restFoot = RestPosition(restWorld, rig.toes[0]) - RestPosition(restWorld, rig.ankle[0]);
    seat.footLength = glm::length(restFoot);
    seat.footRestPitch = std::asin(std::clamp(restFoot.y / std::max(seat.footLength, 1e-4f), -1.0f, 1.0f));

    // The hips slide forward until the hands reach the wheel with the elbows bent; the feet stay on
    // the pedals unless the legs would be straighter than they can push.
    const glm::vec3 seatHips = seat.hips;
    std::vector<ModelNodePose> poses;
    seat.leanDegrees = 0.0f;
    for (float slide = 0.0f; slide <= kMaxSlide + 1e-4f; slide += kSlideStep)
    {
        seat.slide = slide;
        seat.hips = seatHips + forward * slide;
        bool reaches = false;
        for (float lean = slide > 0.0f ? kMaxLeanDegrees : 0.0f; lean <= kMaxLeanDegrees + 1e-3f; lean += kLeanStepDegrees)
        {
            seat.leanDegrees = lean;
            DriverPoseResult result;
            PoseDriver(skeleton, rig, DriverPoseFromSeat(seat, glm::vec3(0.0f), 0.0f, false), poses, &result);
            reaches = std::max(result.armStretch[0], result.armStretch[1]) <= kMaxArmStretch;
            if (reaches)
            {
                break;
            }
        }
        if (reaches)
        {
            break;
        }
    }
    LOG_INFO(
        "Driver's seat: hips at ({:.3f}, {:.3f}, {:.3f}) (slid {:.2f} m forward), back {:.0f} deg, leaning {:.0f} deg off it, cushion {} at {:.3f}, floor {} at {:.3f}, "
        "toe board {} at {:.3f}, steering wheel radius {:.3f}; {} pedals found (accelerator x {:.3f}, brake {:.3f}, clutch {:.3f}, foot rest {:.3f}, at z {:.3f})",
        seat.hips.x, seat.hips.y, seat.hips.z, seat.slide, seat.reclineDegrees, seat.leanDegrees, cushionHit.has_value() ? "found" : "guessed", cushion,
        floorHit.has_value() ? "found" : "guessed", floor, toeBoardFound ? "found" : "guessed", toeBoard, seat.wheelRadius, pedals.size(),
        seat.throttle.x, seat.brake.x, seat.clutch.x, seat.footRest.x, seat.throttle.z);
    return seat;
}

void UpdateDriverFeet(VehicleDriverFeet& feet, float throttle, float brake, float clutch, float deltaSeconds)
{
    const float step = std::max(deltaSeconds, 0.0f) / kFootMoveSeconds;
    const auto moveTowards = [step](float value, float target)
    {
        return value < target ? std::min(value + step, target) : std::max(value - step, target);
    };
    // The right foot goes to the brake for a brake and back to the accelerator for the accelerator;
    // with neither it stays on the brake once there.
    if (brake > kPedalThreshold)
    {
        feet.rightOnBrake = moveTowards(feet.rightOnBrake, 1.0f);
    }
    else if (throttle > kPedalThreshold || feet.rightOnBrake < 1.0f)
    {
        feet.rightOnBrake = moveTowards(feet.rightOnBrake, 0.0f);
    }
    // The left foot goes to the clutch, and stays a moment after it closes, as through a gear change.
    feet.clutchHold = clutch > kPedalThreshold ? kClutchHoldSeconds : std::max(feet.clutchHold - std::max(deltaSeconds, 0.0f), 0.0f);
    feet.leftOnClutch = moveTowards(feet.leftOnClutch, feet.clutchHold > 0.0f ? 1.0f : 0.0f);

    // A pedal is pressed once the foot is on it.
    const float ease = 1.0f - std::exp(-std::max(deltaSeconds, 0.0f) / kPressSeconds);
    const auto press = [ease](float value, float target)
    {
        return value + (target - value) * ease;
    };
    const auto onPedal = [](float there)
    {
        return std::clamp((there - 0.8f) * 5.0f, 0.0f, 1.0f);
    };
    feet.throttle = press(feet.throttle, std::clamp(throttle, 0.0f, 1.0f) * onPedal(1.0f - feet.rightOnBrake));
    feet.brake = press(feet.brake, std::clamp(brake, 0.0f, 1.0f) * onPedal(feet.rightOnBrake));
    feet.clutch = press(feet.clutch, std::clamp(clutch, 0.0f, 1.0f) * onPedal(feet.leftOnClutch));
}

namespace
{
// The ankle for the ball of the foot on `pedal`, pressed `pressed` of the way, and the foot's raise.
std::pair<glm::vec3, float> FootOnPedal(const VehicleDriverSeat& seat, const glm::vec3& pedal, const PedalTravel& travel, float pressed)
{
    const float raise = kFootRaiseDegrees - travel.tipDegrees * pressed;
    const float pitch = seat.footRestPitch + glm::radians(raise);
    const glm::vec3 forward(0.0f, 0.0f, 1.0f);
    const glm::vec3 foot = (forward * std::cos(pitch) + glm::vec3(0.0f, 1.0f, 0.0f) * std::sin(pitch)) * seat.footLength;
    return {pedal - forward * (kSoleThickness - travel.forward * pressed) - foot, raise};
}

float Smooth(float value)
{
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}

DriverPoseInput DriverPoseFromSeat(
    const VehicleDriverSeat& seat,
    const glm::vec3& seatOffset,
    float steeringWheelTurn,
    bool hideHead,
    const VehicleDriverFeet& feet)
{
    DriverPoseInput input;
    // Vehicle space has +X to the car's left.
    input.hips = seat.hips + glm::vec3(-seatOffset.x, seatOffset.y, seatOffset.z);
    input.reclineDegrees = seat.reclineDegrees;
    input.leanDegrees = seat.leanDegrees;
    input.wheelCenter = seat.wheelCenter;
    input.wheelAxis = seat.wheelAxis;
    input.wheelRadius = seat.wheelRadius;
    input.wheelTurn = steeringWheelTurn * seat.wheelTurnSign;
    // The right foot between the accelerator and the brake, the left between the foot rest and the
    // clutch, lifted on the way across.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const std::pair<glm::vec3, float> throttle = FootOnPedal(seat, seat.throttle, kThrottleTravel, feet.throttle);
    const std::pair<glm::vec3, float> brake = FootOnPedal(seat, seat.brake, kBrakeTravel, feet.brake);
    const std::pair<glm::vec3, float> footRest = FootOnPedal(seat, seat.footRest, PedalTravel{0.0f, 0.0f}, 0.0f);
    const std::pair<glm::vec3, float> clutch = FootOnPedal(seat, seat.clutch, kClutchTravel, feet.clutch);
    const float right = Smooth(feet.rightOnBrake);
    const float left = Smooth(feet.leftOnClutch);
    input.ankles[0] = glm::mix(footRest.first, clutch.first, left) + up * (kFootLift * std::sin(glm::pi<float>() * left));
    input.footRaiseDegrees[0] = glm::mix(footRest.second, clutch.second, left);
    input.ankles[1] = glm::mix(throttle.first, brake.first, right) + up * (kFootLift * std::sin(glm::pi<float>() * right));
    input.footRaiseDegrees[1] = glm::mix(throttle.second, brake.second, right);
    // Not past where the legs can push.
    for (size_t side = 0; side < 2; ++side)
    {
        const float outward = side == 0 ? 1.0f : -1.0f;
        glm::vec3& ankle = input.ankles[side];
        const glm::vec3 hip = input.hips + glm::vec3(outward * seat.hipHalfWidth, 0.0f, 0.0f);
        const float across = glm::length(glm::vec2(ankle.x - hip.x, ankle.y - hip.y));
        const float reach = kMaxLegStretch * seat.legLength;
        if (reach > across)
        {
            ankle.z = std::min(ankle.z, hip.z + std::sqrt(reach * reach - across * across));
        }
    }
    // A right turn (positive) looks right, which is a negative yaw.
    input.headYawDegrees = std::clamp(-glm::degrees(input.wheelTurn) * kHeadYawShare, -kMaxHeadYawDegrees, kMaxHeadYawDegrees);
    input.hideHead = hideHead;
    return input;
}

namespace VehicleDriverService
{

void Tick(RendererSharedState& state, float deltaSeconds)
{
    VehicleDriverState& drivers = state.vehicleDrivers;
    drivers.problems.clear();
    if (!state.editorWorld)
    {
        drivers.posed.clear();
        return;
    }
    IEditorWorld& world = state.GetEditorWorld();
    VehicleDriveSession* const session = state.vehicleDrive.session.get();
    bool sessionHasDriver = false;
    std::unordered_set<entt::entity> posed;

    for (const entt::entity entity : world.Registry().view<const ModelComponent>())
    {
        const ModelComponent& model = world.GetModel(entity);
        if (model.driverVehicleUuid.empty())
        {
            continue;
        }
        const entt::entity car = FindEntityById(world, drivers, model.driverVehicleUuid);
        if (car == entt::null || car == entity || !world.HasModelComponent(car))
        {
            drivers.problems[entity] = "its car is not in the scene";
            continue;
        }
        const std::shared_ptr<const LoadedModelData> carData = ModelCache::Get(world.GetModel(car).sourcePath);
        if (!carData)
        {
            drivers.problems[entity] = "its car's model is not loaded";
            continue;
        }

        // The driver's model space is the car's vehicle space: it follows the car.
        const glm::quat vehicleToModel = VehicleDriveService::VehicleToModelRotation(
            carData->wheelRig.has_value() ? VehicleDriveService::ModelFrontFromWheels(*carData->wheelRig) : VehicleModelFront::NegativeZ);
        const glm::mat4 matrix = world.GetModelMatrix(car) * glm::mat4_cast(vehicleToModel);
        if (!NearlyEqual(world.GetModelMatrix(entity), matrix))
        {
            world.ApplyTransformMatrix(entity, matrix);
        }

        const std::shared_ptr<const LoadedModelData> driverData = ModelCache::Get(model.sourcePath);
        const DriverRig* const rig = state.modelAnimation.GetDriverRig(entity);
        if (!driverData || !driverData->skeleton || rig == nullptr)
        {
            drivers.problems[entity] = "it has no humanoid skeleton to sit with";
            continue;
        }
        VehicleDriverState::FittedSeat& fitted = drivers.seats[{carData.get(), driverData.get()}];
        if (!fitted.car)
        {
            fitted.car = carData;
            fitted.driver = driverData;
            fitted.seat = FitDriverSeat(*carData, vehicleToModel, *driverData->skeleton, *rig);
        }
        if (!fitted.seat.has_value())
        {
            drivers.problems[entity] = "its car has no steering wheel (STEER_HR)";
            continue;
        }

        const bool driven = session != nullptr && session->entity == car;
        const bool fromCockpit = driven && state.vehicleDrive.camera.follow && state.vehicleDrive.cameraView == VehicleCameraView::Cockpit;

        // The feet work the pedals the car is driven with.
        VehicleDriverFeet& feet = drivers.feet[entity];
        float throttle = 0.0f;
        float brake = 0.0f;
        float clutch = 0.0f;
        if (driven)
        {
            const VehicleControls& controls = session->controls;
            const VehicleTelemetry telemetry = session->physics->GetVehicleTelemetry(session->vehicle);
            // The throttle drives the way the gear does (either way in neutral); pulled against it, it brakes.
            const float way = telemetry.gear != 0 ? (telemetry.gear > 0 ? 1.0f : -1.0f) : (controls.throttle < 0.0f ? -1.0f : 1.0f);
            throttle = std::max(controls.throttle * way, 0.0f);
            brake = std::max(controls.brake, std::max(-controls.throttle * way, 0.0f));
            clutch = controls.clutchPedal ? 1.0f : std::clamp(1.0f - telemetry.clutch, 0.0f, 1.0f);
        }
        UpdateDriverFeet(feet, throttle, brake, clutch, session != nullptr && session->paused ? 0.0f : deltaSeconds);
        DriverPoseInput input = DriverPoseFromSeat(*fitted.seat, model.driverSeatOffset, driven ? session->steeringWheelTurn : 0.0f, fromCockpit, feet);

        // A gear change: the hand on the lever's side goes to the knob as the lever moves.
        if (driven && carData->gearLever.has_value())
        {
            const float weight = GearShiftHandWeight(session->gearShift);
            if (weight > 0.0f)
            {
                const glm::mat4 lever = GearLeverTransform(*carData->gearLever, GearLeverGatePosition(session->gearShift), vehicleToModel);
                const glm::vec3 knob = glm::conjugate(vehicleToModel) * glm::vec3(lever * glm::vec4(carData->gearLever->knob, 1.0f));
                const size_t side = knob.x >= input.hips.x ? 0 : 1;
                DriverHandHold& hold = input.holds[side];
                hold.grip = knob + glm::vec3(0.0f, kKnobGripAbove, 0.0f);
                hold.direction = glm::normalize(glm::vec3(0.0f, -0.4f, 1.0f));
                hold.palmFacing = glm::normalize(glm::vec3(0.0f, -1.0f, -0.4f));
                hold.weight = weight;
            }
        }
        state.modelAnimation.SetDriverPose(entity, input);
        posed.insert(entity);
        if (driven && !sessionHasDriver)
        {
            if (const DriverPoseResult* const result = state.modelAnimation.GetDriverPoseResult(entity))
            {
                session->driverEyes = result->eyes * session->scale;
                sessionHasDriver = true;
            }
        }
    }

    for (const entt::entity entity : drivers.posed)
    {
        if (posed.count(entity) == 0)
        {
            state.modelAnimation.ClearDriverPose(entity);
        }
    }
    for (auto it = drivers.feet.begin(); it != drivers.feet.end();)
    {
        it = posed.count(it->first) == 0 ? drivers.feet.erase(it) : std::next(it);
    }
    drivers.posed = std::move(posed);
    if (session != nullptr && !sessionHasDriver)
    {
        session->driverEyes.reset();
    }
}

std::string Problem(const RendererSharedState& state, entt::entity entity)
{
    const auto found = state.vehicleDrivers.problems.find(entity);
    return found != state.vehicleDrivers.problems.end() ? found->second : std::string{};
}
}
}
