#pragma once

#include <engine/asset/model_driver_pose.h>
#include <engine/asset/model_loader.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace me
{

struct RendererSharedState;

// Where a driver sits in a car, in the car's vehicle space (+Z forward, +X the car's left, +Y up,
// from the model's origin, in the model's units), as FitDriverSeat finds it.
struct VehicleDriverSeat
{
    glm::vec3 hips{0.0f};
    float reclineDegrees = 15.0f;
    // How far the back bends forward off the seat's (DriverPoseInput::leanDegrees).
    float leanDegrees = 0.0f;
    glm::vec3 wheelCenter{0.0f};
    // The steering column, pointing away from the driver.
    glm::vec3 wheelAxis{0.0f, 0.0f, 1.0f};
    float wheelRadius = 0.18f;
    // +1 when wheelAxis is the model's own steering axis (ModelSteeringWheel::axis), -1 when it was
    // turned round to point away from the driver: the wheel's turn about it has this sign.
    float wheelTurnSign = 1.0f;
    std::array<glm::vec3, 2> ankles{};
    // The driver's eyes the seat was found from: the car's data, else from the steering wheel.
    glm::vec3 carEyes{0.0f};
    // How far forward the hips slid for the hands to reach the wheel (metres).
    float slide = 0.0f;
};

// Fits a humanoid driver (its skeleton and rig) to the car's driver's seat: the seat's cushion and
// back, the floor and the toe board are found by casting rays through the car's meshes round its
// driver's eyes (the car's data, else 0.59 m behind and 0.29 m over its steering wheel), and the hips
// slide forward until the hands reach the wheel with the elbows bent. Nullopt for a car without a
// steering wheel.
std::optional<VehicleDriverSeat> FitDriverSeat(
    const LoadedModelData& car,
    const glm::quat& vehicleToModel,
    const ModelSkeleton& skeleton,
    const DriverRig& rig);

// The driver's pose in the seat, moved by `seatOffset` (metres: to the car's right, up, forward),
// holding the wheel turned `steeringWheelTurn` (radians about ModelSteeringWheel::axis, as
// VehicleDriveSession::steeringWheelTurn). The driver's model space is the car's vehicle space.
DriverPoseInput DriverPoseFromSeat(const VehicleDriverSeat& seat, const glm::vec3& seatOffset, float steeringWheelTurn, bool hideHead);

struct VehicleDriverState
{
    struct FittedSeat
    {
        // Keep the models alive, so the key's addresses are never another model's.
        std::shared_ptr<const LoadedModelData> car;
        std::shared_ptr<const LoadedModelData> driver;
        std::optional<VehicleDriverSeat> seat;
    };
    // By car model and driver model.
    std::map<std::pair<const LoadedModelData*, const LoadedModelData*>, FittedSeat> seats;
    // The entities posed as drivers in the last frame.
    std::unordered_set<entt::entity> posed;
    // Entity ids found before, checked again on use.
    std::unordered_map<std::string, entt::entity> entitiesById;
    // Why a model with a car to drive is not sitting in it; no entry when it is.
    std::unordered_map<entt::entity, std::string> problems;
};

namespace VehicleDriverService
{
// Per frame, after the drive and before the animations: every model seated in a car
// (ModelComponent::driverVehicleUuid) takes the car's transform, its model space the car's vehicle
// space, and a humanoid one sits in the driver's seat holding the steering wheel as the drive turns
// it. While the car is driven from the cockpit, the driver's head is hidden and the camera sits at its
// eyes.
void Tick(RendererSharedState& state);

// Why the entity is not sitting in its car (no such car, no steering wheel, no humanoid skeleton);
// empty when it is, or when it drives none.
std::string Problem(const RendererSharedState& state, entt::entity entity);
}
}
