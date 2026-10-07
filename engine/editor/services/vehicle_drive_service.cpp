#include "vehicle_drive_service.h"

#include <engine/asset/ac_car_data.h>
#include <engine/asset/model_cache.h>
#include <engine/core/input/input.h>
#include <engine/core/log/log.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/vehicle_haptics.h>
#include <engine/logic/world_bounds.h>
#include <engine/physics/collision_filter.h>
#include <engine/physics/vehicle_wheel_motion.h>
#include <engine/renderer/renderer_world.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace me
{

namespace
{
// Keyboard steering eases to full lock in this many seconds, and back to centre faster.
constexpr float kKeyboardSteerSeconds = 0.35f;
constexpr float kKeyboardCentreSeconds = 0.2f;
constexpr float kGamepadStickDeadZone = 0.12f;
// Recover looks for the ground from this far above the car's origin (over a car on its roof, under
// most ceilings) down this far, and sets the car down this much above where it started over the ground.
constexpr float kRecoverRayLift = 1.0f;
constexpr float kRecoverRayLength = 100.0f;
constexpr float kRecoverDropHeight = 0.15f;
// A car placed in the air would otherwise be dropped from as high at every recovery.
constexpr float kRecoverMaxHeightAboveGround = 1.0f;
// The right stick swings the chase camera round the car this fast at full deflection.
constexpr float kGamepadOrbitDegreesPerSecond = 180.0f;
// The ground plane under everything, so a car driven off the edge of the track lands somewhere.
constexpr float kGroundPlaneHalfSize = 5000.0f;
constexpr float kGroundPlaneHalfThickness = 0.5f;
// How far the right mouse button may tilt the chase camera down onto the car and up from below.
constexpr float kOrbitMaxRaiseDegrees = 60.0f;
constexpr float kOrbitMaxLowerDegrees = 10.0f;
// The driver's eyes from the steering wheel's centre, as the R34's DRIVEREYES sit from its STEER_HR,
// and how far they look down when the car's data does not say (Kunos' cars: 0 to 5 degrees).
constexpr float kEyesBehindSteeringWheel = 0.59f;
constexpr float kEyesOverSteeringWheel = 0.29f;
constexpr float kCockpitPitchDegrees = -3.0f;
// Without a steering wheel: the eyes' place in the body's box, by height from its bottom and from its
// middle back along its length (Kunos' cars: 0.80 to 0.83 of the height, 0.04 to 0.13 of the length).
constexpr float kEyesHeightShare = 0.81f;
constexpr float kEyesBehindMiddleShare = 0.08f;
// The bonnet camera: this share of the way from the eyes to the front, this far over the body there,
// looking down this much at the road over the bonnet.
constexpr float kBonnetShareToFront = 0.45f;
constexpr float kBonnetCameraLift = 0.12f;
constexpr float kBonnetPitchDegrees = -1.5f;
// The bumper camera: this far ahead of the front, at this share of the body's height (within these
// heights), level.
constexpr float kBumperCameraAhead = 0.05f;
constexpr float kBumperHeightShare = 0.35f;
constexpr float kBumperMinHeight = 0.3f;
constexpr float kBumperMaxHeight = 0.6f;
// The body's height along its centre line: points this close to it, in bins this long, read over this
// far each side of where it is asked for.
constexpr float kProfileHalfWidth = 0.3f;
constexpr float kProfileBinLength = 0.05f;
constexpr float kProfileWindow = 0.1f;
// A camera on the body may be inside it, close to the dashboard and the pillars.
constexpr float kMountedNearPlane = 0.05f;

float MoveTowards(float value, float target, float maxDelta)
{
    if (std::abs(target - value) <= maxDelta)
    {
        return target;
    }
    return value + (target > value ? maxDelta : -maxDelta);
}

float ApplyDeadZone(float value, float deadZone)
{
    const float magnitude = std::abs(value);
    if (magnitude <= deadZone)
    {
        return 0.0f;
    }
    return std::copysign((magnitude - deadZone) / (1.0f - deadZone), value);
}

bool IsEitherKeyDown(const InputState& input, SDL_Scancode first, SDL_Scancode second)
{
    return input.IsKeyDown(KeyCode(first)) || input.IsKeyDown(KeyCode(second));
}

// The entity's pose without scale, and the scale, from its model matrix.
PhysicsPose DecomposePose(const glm::mat4& matrix, glm::vec3& scale)
{
    scale = glm::vec3(glm::length(glm::vec3(matrix[0])), glm::length(glm::vec3(matrix[1])), glm::length(glm::vec3(matrix[2])));
    scale = glm::max(scale, glm::vec3(WorldUnits::kMinimumScale));
    const glm::mat3 rotation(
        glm::vec3(matrix[0]) / scale.x,
        glm::vec3(matrix[1]) / scale.y,
        glm::vec3(matrix[2]) / scale.z);

    PhysicsPose pose;
    pose.position = glm::vec3(matrix[3]);
    pose.rotation = glm::normalize(glm::quat_cast(rotation));
    return pose;
}

glm::mat4 ComposeMatrix(const PhysicsPose& pose, const glm::vec3& scale)
{
    return glm::translate(glm::mat4(1.0f), pose.position) * glm::mat4_cast(pose.rotation) * glm::scale(glm::mat4(1.0f), scale);
}

// The model's wheels as a layout in vehicle space, scaled as the entity is.
VehicleWheelLayout BuildWheelLayout(const ModelWheelRig& rig, const glm::quat& vehicleToModel, const glm::vec3& scale)
{
    const glm::quat modelToVehicle = glm::conjugate(vehicleToModel);
    VehicleWheelLayout layout{};
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const ModelWheelRig::Corner& corner = rig.corners[index];
        layout[index].center = modelToVehicle * (corner.center * scale);
        layout[index].radius = corner.radius * (scale.y + scale.z) * 0.5f;
        layout[index].width = corner.width * scale.x;
    }
    return layout;
}

void RestoreCamera(Camera& camera, const Camera& saved)
{
    // Only where it was and where it looked: exposure and the lens stay as the user left them.
    camera.position = saved.position;
    camera.yawDegrees = saved.yawDegrees;
    camera.pitchDegrees = saved.pitchDegrees;
}

// The lens of a view on the body: its field of view, a near plane close enough for the dashboard, and
// the body's up (UpdateMountedCamera). What it replaces is kept to put back.
void ApplyMountedLens(Camera& camera, VehicleDriveSession& session, float fovDegrees)
{
    if (!session.mountedLensApplied)
    {
        session.fovBeforeMounted = camera.fovDegrees;
        session.nearPlaneBeforeMounted = camera.nearPlane;
        session.upBeforeMounted = camera.worldUp;
        session.mountedLensApplied = true;
    }
    camera.fovDegrees = std::clamp(fovDegrees, WorldUnits::kUiCameraFovMinDegrees, WorldUnits::kUiCameraFovMaxDegrees);
    camera.nearPlane = std::min(session.nearPlaneBeforeMounted, kMountedNearPlane);
}

void RestoreMountedLens(Camera& camera, VehicleDriveSession& session)
{
    if (!session.mountedLensApplied)
    {
        return;
    }
    camera.fovDegrees = session.fovBeforeMounted;
    camera.nearPlane = session.nearPlaneBeforeMounted;
    camera.worldUp = session.upBeforeMounted;
    session.mountedLensApplied = false;
}

// The highest point of the car's own surfaces near its centre line (x = centreX), by distance along it,
// in vehicle space: what the bonnet camera sits over.
std::function<std::optional<float>(float)> BodyHeightProfile(
    const RendererWorld& renderWorld, entt::entity entity, const glm::quat& modelToVehicle, const glm::vec3& scale, float centreX)
{
    std::map<int, float> highest;
    for (const std::shared_ptr<const CpuRenderSubmesh>& entry : renderWorld.GetRenderSubmeshes())
    {
        const CpuRenderSubmesh& submesh = *entry;
        if (submesh.entity != entity || !submesh.mesh || submesh.decal || submesh.water)
        {
            continue;
        }
        for (const Vertex& vertex : submesh.mesh->vertices)
        {
            const glm::vec3 point = modelToVehicle * (glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]) * scale);
            if (std::abs(point.x - centreX) > kProfileHalfWidth)
            {
                continue;
            }
            const auto [bin, inserted] = highest.try_emplace(static_cast<int>(std::floor(point.z / kProfileBinLength)), point.y);
            if (!inserted)
            {
                bin->second = std::max(bin->second, point.y);
            }
        }
    }
    return [highest = std::move(highest)](float z) -> std::optional<float>
    {
        std::optional<float> top;
        const auto end = highest.upper_bound(static_cast<int>(std::floor((z + kProfileWindow) / kProfileBinLength)));
        for (auto bin = highest.lower_bound(static_cast<int>(std::floor((z - kProfileWindow) / kProfileBinLength))); bin != end; ++bin)
        {
            top = top.has_value() ? std::max(*top, bin->second) : bin->second;
        }
        return top;
    };
}

// The seat offset (right, up, forward) in vehicle space, where +X is the car's left.
glm::vec3 SeatOffsetInVehicleSpace(const glm::vec3& seatOffset)
{
    return glm::vec3(-seatOffset.x, seatOffset.y, seatOffset.z);
}
}

const char* VehicleCameraViewName(VehicleCameraView view)
{
    switch (view)
    {
    case VehicleCameraView::Chase:
        return "Chase";
    case VehicleCameraView::Cockpit:
        return "Cockpit";
    case VehicleCameraView::Bonnet:
        return "Bonnet";
    case VehicleCameraView::Bumper:
        return "Bumper";
    }
    return "Chase";
}

VehicleCameraView NextVehicleCameraView(VehicleCameraView view)
{
    return static_cast<VehicleCameraView>((static_cast<size_t>(view) + 1) % kVehicleCameraViewCount);
}

namespace VehicleDriveService
{

glm::quat VehicleToModelRotation(VehicleModelFront front)
{
    // A model facing -Z is half a turn about Y from vehicle space (its own inverse).
    return front == VehicleModelFront::NegativeZ ? glm::quat(0.0f, 0.0f, 1.0f, 0.0f) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

VehicleModelFront ModelFrontFromWheels(const ModelWheelRig& rig)
{
    const float frontZ = (rig.corners[0].center.z + rig.corners[1].center.z) * 0.5f;
    const float rearZ = (rig.corners[2].center.z + rig.corners[3].center.z) * 0.5f;
    return frontZ < rearZ ? VehicleModelFront::NegativeZ : VehicleModelFront::PositiveZ;
}

namespace
{
// Puts the camera where the view the car is seen from has it, looking round by the session's orbit.
// With deltaSeconds of zero the chase camera jumps straight to its place.
void PlaceCamera(RendererSharedState& state, VehicleDriveSession& session, const PhysicsPose& pose, float deltaSeconds)
{
    const VehicleCameraSettings& settings = state.vehicleDrive.camera;
    const VehicleCameraView view = state.vehicleDrive.cameraView;
    if (view == VehicleCameraView::Chase)
    {
        RestoreMountedLens(state.camera, session);
        UpdateChaseCamera(state.camera, pose, settings, deltaSeconds, session.orbit);
        return;
    }
    ApplyMountedLens(state.camera, session, view == VehicleCameraView::Cockpit ? settings.cockpitFovDegrees : settings.exteriorFovDegrees);
    VehicleCameraMount mount = session.cameraMounts[static_cast<size_t>(view) - 1];
    if (view == VehicleCameraView::Cockpit)
    {
        // A character driving the car sees through its own eyes.
        if (session.driverEyes.has_value())
        {
            mount.position = *session.driverEyes;
        }
        mount.position += SeatOffsetInVehicleSpace(settings.seatOffset);
    }
    UpdateMountedCamera(state.camera, pose, mount, session.orbit);
}
}

VehicleSettings DefaultTuning()
{
    VehicleSettings tuning;
    tuning.tyreModel = VehicleTyreModel::Brush;
    // The brakes by the car's own front/rear split (and its ABS), as the game has them.
    tuning.dynamicBrakeBias = false;
    return tuning;
}

void Start(RendererSharedState& state, entt::entity entity, const VehicleSettings& tuning)
{
    IEditorWorld& world = state.GetEditorWorld();
    if (!world.HasModelComponent(entity))
    {
        throw std::runtime_error("select the car's model in the scene to drive it");
    }
    Stop(state);

    const auto buildStart = std::chrono::steady_clock::now();
    auto session = std::make_unique<VehicleDriveSession>();
    session->entity = entity;
    session->name = world.GetTag(entity).name;
    session->startTransform = world.GetTransform(entity);
    session->startPose = DecomposePose(world.GetModelMatrix(entity), session->scale);
    // A model that names its wheels says which way it faces; the tuning's guess is for the rest.
    std::shared_ptr<const LoadedModelData> modelData = ModelCache::Get(world.GetModel(entity).sourcePath);
    const ModelWheelRig* rig = modelData && modelData->wheelRig.has_value() ? &*modelData->wheelRig : nullptr;
    VehicleModelFront modelFront = tuning.modelFront;
    if (rig != nullptr)
    {
        modelFront = ModelFrontFromWheels(*rig);
    }
    session->vehicleToModel = VehicleToModelRotation(modelFront);
    session->startPose.rotation = glm::normalize(session->startPose.rotation * glm::conjugate(session->vehicleToModel));
    session->cameraBeforeDriving = state.camera;

    // The car is fitted to its model's bounds, scaled as the entity is, turned into vehicle space
    // (an axis-aligned turn, so two corners are enough).
    const ModelBoundsComponent& bounds = world.GetModelBounds(entity);
    const glm::vec3 localMin = bounds.hasBounds ? bounds.minBounds : WorldUnits::kDefaultCubeMinBoundsMeters;
    const glm::vec3 localMax = bounds.hasBounds ? bounds.maxBounds : WorldUnits::kDefaultCubeMaxBoundsMeters;
    const glm::quat modelToVehicle = glm::conjugate(session->vehicleToModel);
    const glm::vec3 cornerA = modelToVehicle * (localMin * session->scale);
    const glm::vec3 cornerB = modelToVehicle * (localMax * session->scale);
    VehicleWheelLayout wheelLayout{};
    if (rig != nullptr)
    {
        wheelLayout = BuildWheelLayout(*rig, session->vehicleToModel, session->scale);
    }
    // A model that carries its car's own figures drives on them, unless the tuning asks otherwise.
    VehicleSettings fitTuning = tuning;
    if (tuning.useCarData && modelData && modelData->carSpec.has_value())
    {
        fitTuning = ApplyCarSpec(tuning, *modelData->carSpec);
        session->carData = DescribeCarSpec(*modelData->carSpec);
        // The body's shell is in the model's frame: scaled and turned into vehicle space as the bounds are.
        fitTuning.chassisHull.clear();
        for (const glm::vec3& point : modelData->carSpec->colliderHull)
        {
            fitTuning.chassisHull.push_back(modelToVehicle * (point * session->scale));
        }
    }
    fitTuning.modelFront = modelFront;
    if (rig != nullptr || (modelData && modelData->steeringWheel.has_value()))
    {
        VehicleWheelAnimation animation;
        animation.model = modelData;
        animation.maxSteerDegrees = std::max(fitTuning.maxSteerAngleDegrees, 1.0f);
        if (tuning.useCarData && modelData && modelData->carSpec.has_value() && modelData->carSpec->steeringWheelLockDegrees.has_value())
        {
            animation.steeringWheelLockDegrees = *modelData->carSpec->steeringWheelLockDegrees;
        }
        if (rig != nullptr)
        {
            for (size_t index = 0; index < kModelWheelCornerCount; ++index)
            {
                animation.restCenters[index] = rig->corners[index].center;
            }
        }
        session->wheels = std::move(animation);
    }
    const VehicleSettings settings = FitVehicleSettingsToBounds(
        glm::min(cornerA, cornerB), glm::max(cornerA, cornerB), fitTuning, rig != nullptr ? &wheelLayout : nullptr);

    // The cameras on the body: the driver's eyes from the car's data (an Assetto Corsa car's DRIVEREYES),
    // else from its steering wheel; the bonnet's over the body's own surfaces.
    {
        const glm::vec3 boundsMin = glm::min(cornerA, cornerB);
        const glm::vec3 boundsMax = glm::max(cornerA, cornerB);
        std::optional<VehicleCameraMount> eyes;
        if (modelData && modelData->carSpec.has_value() && modelData->carSpec->cockpitCamera.has_value())
        {
            eyes = *modelData->carSpec->cockpitCamera;
            eyes->position = modelToVehicle * (eyes->position * session->scale);
        }
        std::optional<glm::vec3> steeringWheel;
        if (modelData && modelData->steeringWheel.has_value())
        {
            steeringWheel = modelToVehicle * (modelData->steeringWheel->center * session->scale);
        }
        session->cameraMounts = ComputeCameraMounts(
            boundsMin, boundsMax, eyes, steeringWheel,
            BodyHeightProfile(state.rendererWorld, entity, modelToVehicle, session->scale, (boundsMin.x + boundsMax.x) * 0.5f));
        const glm::vec3& cockpit = session->cameraMounts[0].position;
        LOG_INFO(
            "'{}': the driver's eyes at ({:.2f}, {:.2f}, {:.2f}) from {}",
            session->name, cockpit.x, cockpit.y, cockpit.z,
            eyes.has_value() ? "the car's data" : steeringWheel.has_value() ? "its steering wheel"
                                                                            : "its bounds");
    }

    session->engineMinRpm = settings.minRpm;
    session->engineMaxRpm = settings.maxRpm;
    session->maxSteerDegrees = settings.maxSteerAngleDegrees;
    session->wheelbase = std::max(settings.frontAxleZ - settings.rearAxleZ, 0.5f);
    session->frontPeakSlipDegrees = settings.frontTyres.peakSlipAngleDegrees > 0.0f ? settings.frontTyres.peakSlipAngleDegrees : 7.0f;
    session->absFitted = settings.useAbs && settings.absSlipRatioLimit > 0.0f;
    session->tractionControlFitted = (settings.useTractionControl && settings.tcSlipRatioLimit > 0.0f) || settings.tractionControlGrip > 0.0f;
    session->turbo = !settings.turbos.empty();
    if (tuning.useCarData && modelData && modelData->carSpec.has_value())
    {
        const VehicleCarSpec& spec = *modelData->carSpec;
        if (spec.defaultTyreCompound.has_value() && *spec.defaultTyreCompound >= 0 &&
            static_cast<size_t>(*spec.defaultTyreCompound) < spec.tyreCompounds.size())
        {
            const VehicleTyreCompound& compound = spec.tyreCompounds[static_cast<size_t>(*spec.defaultTyreCompound)];
            session->frontTyre = compound.front.shortName;
            session->rearTyre = compound.rear.shortName;
        }
    }

    glm::vec3 carWorldMin = session->startPose.position;
    glm::vec3 carWorldMax = session->startPose.position;
    ComputeWorldModelBounds(world, entity, carWorldMin, carWorldMax);

    session->physics = std::make_unique<PhysicsWorld>();
    const float lowestGeometry = AddSceneCollision(*session->physics, state.rendererWorld, world, entity, carWorldMin.y);
    const float groundY = std::min(lowestGeometry, carWorldMin.y);
    session->physics->AddStaticBox(
        glm::vec3(session->startPose.position.x, groundY - kGroundPlaneHalfThickness, session->startPose.position.z),
        glm::vec3(kGroundPlaneHalfSize, kGroundPlaneHalfThickness, kGroundPlaneHalfSize));
    session->vehicle = session->physics->AddVehicle(settings, session->startPose);
    if (const std::optional<float> ground = session->physics->FindGroundBelow(
            session->startPose.position + glm::vec3(0.0f, kRecoverRayLift, 0.0f), kRecoverRayLength))
    {
        session->startHeightAboveGround = std::clamp(session->startPose.position.y - *ground, 0.0f, kRecoverMaxHeightAboveGround);
    }

    const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
    if (!session->carData.empty())
    {
        LOG_INFO("Driving '{}' on the car's own data: {}", session->name, session->carData);
    }
    LOG_INFO(
        "Driving '{}' on {}: {} static bodies, {} collision triangles, built in {:.0f} ms",
        session->name,
        rig != nullptr ? "the wheels its nodes define" : "wheels fitted to its bounds",
        session->physics->GetStaticBodyCount(),
        session->physics->GetStaticTriangleCount(),
        buildMs);

    if (state.vehicleDrive.camera.follow)
    {
        PlaceCamera(state, *session, session->startPose, 0.0f);
    }
    state.vehicleDrive.session = std::move(session);
    state.vehicleDrive.lastError.clear();
}

void Stop(RendererSharedState& state)
{
    std::unique_ptr<VehicleDriveSession> session = std::move(state.vehicleDrive.session);
    if (!session)
    {
        return;
    }

    state.input.ClearGamepadFeedback();
    state.rendererWorld.ClearSubmeshLocalTransforms(session->entity);
    IEditorWorld& world = state.GetEditorWorld();
    if (world.IsValidEntity(session->entity) && world.Registry().all_of<TransformComponent>(session->entity))
    {
        world.EditTransform(session->entity) = session->startTransform;
        world.MarkTransformDirty(session->entity);
    }
    RestoreMountedLens(state.camera, *session);
    if (state.vehicleDrive.camera.follow)
    {
        RestoreCamera(state.camera, session->cameraBeforeDriving);
    }
    LOG_INFO("Stopped driving '{}'", session->name);
}

void Reset(RendererSharedState& state)
{
    if (VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        session->physics->ResetVehicle(session->vehicle, session->startPose);
        session->keyboardSteering = 0.0f;
        session->steeringAssist = VehicleSteeringAssistState{};
        session->haptics = VehicleHapticsState{};
    }
}

void Recover(RendererSharedState& state)
{
    VehicleDriveSession* session = state.vehicleDrive.session.get();
    if (session == nullptr)
    {
        return;
    }
    const PhysicsPose current = session->physics->GetVehiclePose(session->vehicle);
    // The heading on the ground. A car on its nose or tail has none: its roof points where it was going
    // (nose down) or back (nose up).
    const glm::vec3 forward = current.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 heading(forward.x, 0.0f, forward.z);
    if (glm::length(heading) < 0.1f)
    {
        const glm::vec3 up = current.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        heading = glm::vec3(up.x, 0.0f, up.z) * (forward.y < 0.0f ? 1.0f : -1.0f);
    }
    const float yaw = glm::length(heading) > 1.0e-4f ? std::atan2(heading.x, heading.z) : 0.0f;

    PhysicsPose pose;
    pose.rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    pose.position = current.position;
    if (const std::optional<float> ground =
            session->physics->FindGroundBelow(current.position + glm::vec3(0.0f, kRecoverRayLift, 0.0f), kRecoverRayLength))
    {
        pose.position.y = *ground + session->startHeightAboveGround + kRecoverDropHeight;
    }
    else
    {
        pose.position.y += kRecoverRayLift;
    }
    session->physics->ResetVehicle(session->vehicle, pose);
    session->keyboardSteering = 0.0f;
    session->haptics = VehicleHapticsState{};
    LOG_INFO("Put '{}' back on its wheels at ({:.1f}, {:.1f}, {:.1f})", session->name, pose.position.x, pose.position.y, pose.position.z);
}

void SetPaused(RendererSharedState& state, bool paused)
{
    if (VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        session->paused = paused;
    }
}

void SetCameraView(RendererSharedState& state, VehicleCameraView view)
{
    state.vehicleDrive.cameraView = view;
    if (VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        // GT7 changes view looking ahead again.
        session->orbit = VehicleCameraOrbit{};
    }
}

void SetBrushTyreBristles(RendererSharedState& state, int ribs, int segmentsPerRib)
{
    if (VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        session->physics->SetVehicleBrushTyreBristles(session->vehicle, ribs, segmentsPerRib);
    }
}

void Step(RendererSharedState& state)
{
    if (VehicleDriveSession* session = state.vehicleDrive.session.get(); session != nullptr && session->paused)
    {
        session->stepRequested = true;
    }
}

// Opens a DualSense's audio haptics once the pad is the one driving, and lets them go when it is not,
// when the settings turn them off, or when the device went away (the cable pulled).
static void UpdateAudioHaptics(RendererSharedState& state, VehicleDriveSession& session, bool dualSense)
{
    const VehicleHapticsSettings& settings = state.vehicleDrive.haptics;
    if (!dualSense)
    {
        session.audioHaptics.reset();
        session.audioHapticsTried = false;
        return;
    }
    if (!settings.enabled || !settings.audioHaptics)
    {
        session.audioHaptics.reset();
        // Turned back on, it is looked for again.
        session.audioHapticsTried = false;
        return;
    }
    if (session.audioHaptics && !session.audioHaptics->IsRunning())
    {
        LOG_WARN("Gamepad haptics: '{}' stopped; back to the rumble emulation", session.audioHaptics->DeviceName());
        session.audioHaptics.reset();
    }
    if (!session.audioHaptics && !session.audioHapticsTried)
    {
        session.audioHapticsTried = true;
        std::string error;
        session.audioHaptics = GamepadHaptics::Open(error);
        if (!session.audioHaptics)
        {
            LOG_INFO("Gamepad haptics unavailable, using the rumble emulation: {}", error);
        }
        session.audioHapticsState = VehicleAudioHapticsState{};
    }
}

// The engine, road and tyres on a DualSense's actuators.
static void PlayAudioHaptics(RendererSharedState& state, VehicleDriveSession& session, uint32_t player, float deltaSeconds)
{
    VehicleAudioHapticsInput input;
    input.telemetry = session.physics->GetVehicleTelemetry(session.vehicle);
    input.wheels = session.physics->GetVehicleWheels(session.vehicle);
    input.body = session.physics->GetVehiclePose(session.vehicle);
    input.minRpm = session.engineMinRpm;
    input.maxRpm = session.engineMaxRpm;
    input.cylinders = session.engineCylinders;
    input.rightTrigger = state.input.GetGamepadAxis(GamepadAxis::RightTrigger, player);
    const VehicleAudioHaptics haptics = ComputeVehicleAudioHaptics(state.vehicleDrive.haptics, input, session.audioHapticsState, deltaSeconds);
    session.audioHaptics->SetVoices(haptics.voices);
    for (size_t side = 0; side < kHapticsSides; ++side)
    {
        if (haptics.kicks[side] > 0.0f)
        {
            session.audioHaptics->Kick(side, haptics.kicks[side]);
        }
    }
}

// Gives the gamepad its engine rumble, gear thump and trigger effects; frees it while the drive is paused,
// typed over, or the window is not the one in focus (the pad's axes freeze then, and so would the effects).
static void UpdateGamepadFeedback(RendererSharedState& state, VehicleDriveSession& session, float deltaSeconds, bool keyboardCaptured)
{
    InputState& input = state.input;
    const int gamepadIndex = input.GetFirstConnectedGamepadIndex();
    if (gamepadIndex < 0)
    {
        UpdateAudioHaptics(state, session, false);
        return;
    }

    const uint32_t player = static_cast<uint32_t>(gamepadIndex);
    const bool dualSense = input.GetGamepadType(player) == SDL_GAMEPAD_TYPE_PS5;
    UpdateAudioHaptics(state, session, dualSense);
    if (session.paused || keyboardCaptured || SDL_GetKeyboardFocus() == nullptr)
    {
        if (session.audioHaptics)
        {
            session.audioHaptics->SetVoices(HapticsVoices{});
        }
        input.SetGamepadFeedback(player, GamepadFeedback{});
        return;
    }
    if (session.audioHaptics)
    {
        PlayAudioHaptics(state, session, player, deltaSeconds);
    }

    VehicleHapticsInput haptics;
    haptics.telemetry = session.physics->GetVehicleTelemetry(session.vehicle);
    haptics.minRpm = session.engineMinRpm;
    haptics.maxRpm = session.engineMaxRpm;
    haptics.rightTrigger = input.GetGamepadAxis(GamepadAxis::RightTrigger, player);
    haptics.leftTrigger = input.GetGamepadAxis(GamepadAxis::LeftTrigger, player);
    haptics.adaptiveTriggers = dualSense;
    GamepadFeedback feedback = ComputeVehicleFeedback(state.vehicleDrive.haptics, haptics, session.haptics, deltaSeconds);
    if (session.audioHaptics)
    {
        // The actuators play the audio; the rumble emulation would take them back.
        feedback.audioHaptics = true;
        feedback.lowFrequencyMotor = 0.0f;
        feedback.highFrequencyMotor = 0.0f;
    }
    input.SetGamepadFeedback(player, feedback);
}

bool Tick(RendererSharedState& state, float deltaSeconds, bool keyboardCaptured)
{
    VehicleDriveSession* session = state.vehicleDrive.session.get();
    if (session == nullptr)
    {
        return false;
    }

    IEditorWorld& world = state.GetEditorWorld();
    if (!world.HasModelComponent(session->entity))
    {
        // Deleted, or the scene was replaced under it: there is nothing left to put back.
        LOG_WARN("Stopped driving '{}': its entity is gone", session->name);
        RestoreMountedLens(state.camera, *session);
        state.rendererWorld.ClearSubmeshLocalTransforms(session->entity);
        state.input.ClearGamepadFeedback();
        state.vehicleDrive.session.reset();
        return false;
    }

    const std::optional<VehicleControls>& scripted = state.vehicleDrive.scriptedControls;
    if (scripted.has_value())
    {
        deltaSeconds = 1.0f / 60.0f;
    }
    VehicleControls controls = scripted.has_value()
                                   ? *scripted
                                   : ReadVehicleControls(
                                         state.input, keyboardCaptured, deltaSeconds, session->keyboardSteering,
                                         state.vehicleDrive.manualGearbox, &session->gearButtonsHeld);
    // The steering assist holds still while the simulation does.
    if (!scripted.has_value())
    {
        const VehicleTelemetry telemetry = session->physics->GetVehicleTelemetry(session->vehicle);
        VehicleSteeringAssistInput assist;
        assist.request = controls.steering;
        assist.forwardSpeed = telemetry.forwardSpeed;
        assist.rightSpeed = telemetry.rightSpeed;
        assist.frontRightSpeed = telemetry.frontAxleRightSpeed;
        assist.maxSteerDegrees = session->maxSteerDegrees;
        assist.wheelbase = session->wheelbase;
        assist.peakSlipDegrees = session->frontPeakSlipDegrees;
        controls.steering = ComputeAssistedSteering(
            state.vehicleDrive.steeringAssist, assist, session->steeringAssist, session->paused ? 0.0f : deltaSeconds);
    }
    const bool resetDown =
        (!keyboardCaptured && state.input.IsKeyDown(KeyCode(SDL_SCANCODE_BACKSPACE))) ||
        (state.input.GetFirstConnectedGamepadIndex() >= 0 && !keyboardCaptured &&
         state.input.IsGamepadButtonDown(GamepadButton::Back, static_cast<uint32_t>(state.input.GetFirstConnectedGamepadIndex())));
    if (resetDown && !session->resetHeld)
    {
        Reset(state);
    }
    session->resetHeld = resetDown;
    const bool recoverDown =
        !keyboardCaptured &&
        (state.input.IsKeyDown(KeyCode(SDL_SCANCODE_R)) ||
         (state.input.GetFirstConnectedGamepadIndex() >= 0 &&
          state.input.IsGamepadButtonDown(GamepadButton::North, static_cast<uint32_t>(state.input.GetFirstConnectedGamepadIndex()))));
    if (recoverDown && !session->recoverHeld && !scripted.has_value())
    {
        Recover(state);
    }
    session->recoverHeld = recoverDown;
    // V, or the right stick's click, changes the view: chase, cockpit, bonnet, bumper and round again.
    const bool viewDown =
        !keyboardCaptured &&
        (state.input.IsKeyDown(KeyCode(SDL_SCANCODE_V)) ||
         (state.input.GetFirstConnectedGamepadIndex() >= 0 &&
          state.input.IsGamepadButtonDown(GamepadButton::RightStick, static_cast<uint32_t>(state.input.GetFirstConnectedGamepadIndex()))));
    if (viewDown && !session->viewButtonHeld)
    {
        SetCameraView(state, NextVehicleCameraView(state.vehicleDrive.cameraView));
    }
    session->viewButtonHeld = viewDown;

    session->physics->SetVehicleControls(session->vehicle, controls);
    session->controls = controls;
    if (!session->paused)
    {
        // At most this long on physics a frame, so a world too slow for real time slows down rather
        // than the frame rate.
        constexpr float kPhysicsBudgetSeconds = 0.025f;
        // A scripted drive runs every step, so it is the same however slowly the frames come.
        const auto stepStart = std::chrono::steady_clock::now();
        const int steps = session->physics->Update(deltaSeconds, scripted.has_value() ? 0.0f : kPhysicsBudgetSeconds);
        // The odometer runs on simulated time, as the car moves.
        session->odometerMetres += std::abs(static_cast<double>(session->physics->GetVehicleTelemetry(session->vehicle).forwardSpeed)) *
                                   steps * PhysicsWorld::kFixedStepSeconds;
        session->scriptedPhysicsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stepStart).count();
        if (deltaSeconds > 0.0f)
        {
            const float share = std::min(steps * PhysicsWorld::kFixedStepSeconds / deltaSeconds, 1.0f);
            const float blend = 1.0f - std::exp(-deltaSeconds / 1.0f);
            session->realTimeShare += (share - session->realTimeShare) * blend;
        }
    }
    else if (session->stepRequested)
    {
        session->physics->Update(1.0f / 60.0f); // one 60 Hz frame's worth of steps
    }
    session->stepRequested = false;

    UpdateGamepadFeedback(state, *session, deltaSeconds, keyboardCaptured);

    const PhysicsPose pose = session->physics->GetVehiclePose(session->vehicle);
    if (scripted.has_value() && !session->paused)
    {
        if (session->scriptedSeconds >= session->nextScriptedLogSeconds)
        {
            const VehicleTelemetry telemetry = session->physics->GetVehicleTelemetry(session->vehicle);
            const glm::vec3 up = pose.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
            LOG_INFO(
                "Test drive {:.0f} s: at ({:.2f}, {:.2f}, {:.2f}), {:.1f} km/h, gear {}, {} wheels on the ground, tilted {:.1f} deg, "
                "{:.0f} ms of physics for the last second{}",
                session->scriptedSeconds,
                pose.position.x,
                pose.position.y,
                pose.position.z,
                telemetry.forwardSpeed * 3.6f,
                telemetry.gear,
                telemetry.wheelsInContact,
                glm::degrees(std::acos(std::clamp(up.y, -1.0f, 1.0f))),
                session->scriptedPhysicsMs,
                telemetry.submergedShare > 0.0f || telemetry.flooded > 0.0f
                    ? fmt::format(
                          "; in water: {:.0f}% under, {:.0f}% full{}",
                          telemetry.submergedShare * 100.0f,
                          telemetry.flooded * 100.0f,
                          telemetry.engineDrowned ? ", engine drowned" : "")
                    : std::string{});
            session->nextScriptedLogSeconds += 1.0f;
            session->scriptedPhysicsMs = 0.0;
        }
        session->scriptedSeconds += deltaSeconds;
    }
    PhysicsPose modelPose = pose;
    modelPose.rotation = pose.rotation * session->vehicleToModel;
    world.ApplyTransformMatrix(session->entity, ComposeMatrix(modelPose, session->scale));
    if (session->wheels.has_value())
    {
        session->steeringWheelTurn = SteeringWheelTurn(
            *session->wheels, pose, session->physics->GetVehicleWheels(session->vehicle), session->vehicleToModel, session->scale);
        state.rendererWorld.SetSubmeshLocalTransforms(
            session->entity,
            BuildWheelSubmeshTransforms(
                *session->wheels,
                pose,
                session->physics->GetVehicleWheels(session->vehicle),
                session->vehicleToModel,
                session->scale));
    }
    if (!state.vehicleDrive.camera.follow)
    {
        RestoreMountedLens(state.camera, *session);
    }
    else
    {
        // Holding the right mouse button looks around the car: dragging right swings the camera to its left side, as if turning the view to the right.
        // From a camera on the body it turns the head the same way: dragging right looks right, dragging down looks down.
        // The right stick does the same, pushed right like a drag to the right and down like a drag down.
        float lookYaw = 0.0f;
        float lookPitch = 0.0f;
        bool lookHeld = state.input.IsMouseLookActive();
        if (lookHeld)
        {
            lookYaw = -state.input.GetMouseDeltaX() * state.camera.mouseSensitivity;
            lookPitch = state.input.GetMouseDeltaY() * state.camera.mouseSensitivity;
        }
        const int gamepadIndex = state.input.GetFirstConnectedGamepadIndex();
        if (!keyboardCaptured && gamepadIndex >= 0)
        {
            const uint32_t player = static_cast<uint32_t>(gamepadIndex);
            const float stickX = state.input.GetGamepadAxis(GamepadAxis::RightX, player);
            const float stickY = state.input.GetGamepadAxis(GamepadAxis::RightY, player);
            if (stickX != 0.0f || stickY != 0.0f)
            {
                lookHeld = true;
                lookYaw -= stickX * kGamepadOrbitDegreesPerSecond * deltaSeconds;
                lookPitch += stickY * kGamepadOrbitDegreesPerSecond * deltaSeconds;
            }
        }
        const VehicleCameraSettings& camera = state.vehicleDrive.camera;
        const bool chase = state.vehicleDrive.cameraView == VehicleCameraView::Chase;
        UpdateCameraOrbit(
            session->orbit, lookHeld, lookYaw, lookPitch, chase ? camera.lookRecenterRate : camera.headLookRecenterRate, deltaSeconds);
        PlaceCamera(state, *session, pose, deltaSeconds);
    }
    return true;
}

VehicleDriveStatus GetStatus(const RendererSharedState& state)
{
    VehicleDriveStatus status;
    status.lastError = state.vehicleDrive.lastError;
    status.cameraView = state.vehicleDrive.cameraView;
    if (const VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        status.active = true;
        status.paused = session->paused;
        status.vehicleName = session->name;
        status.carData = session->carData;
        status.telemetry = session->physics->GetVehicleTelemetry(session->vehicle);
        status.pose = session->physics->GetVehiclePose(session->vehicle);
        status.wheels = session->physics->GetVehicleWheels(session->vehicle);
        status.linkage = session->physics->GetVehicleLinkage(session->vehicle);
        status.staticBodyCount = session->physics->GetStaticBodyCount();
        status.staticTriangleCount = session->physics->GetStaticTriangleCount();
        status.realTimeShare = session->realTimeShare;
        status.controls = session->controls;
        status.manualGearbox = state.vehicleDrive.manualGearbox;
        status.engineMaxRpm = session->engineMaxRpm;
        status.absFitted = session->absFitted;
        status.tractionControlFitted = session->tractionControlFitted;
        status.counterSteerAssist = state.vehicleDrive.steeringAssist.enabled && state.vehicleDrive.steeringAssist.counterSteerAssist;
        status.turbo = session->turbo;
        status.frontTyre = session->frontTyre;
        status.rearTyre = session->rearTyre;
        status.odometerMetres = session->odometerMetres;
    }
    return status;
}

void RunWithVehicleAtStart(RendererSharedState& state, const std::function<void()>& action)
{
    const VehicleDriveSession* session = state.vehicleDrive.session.get();
    IEditorWorld& world = state.GetEditorWorld();
    if (session == nullptr || !world.HasModelComponent(session->entity))
    {
        action();
        return;
    }

    TransformComponent& transform = world.EditTransform(session->entity);
    const TransformComponent driven = transform;
    transform = session->startTransform;
    try
    {
        action();
    }
    catch (...)
    {
        world.EditTransform(session->entity) = driven;
        throw;
    }
    world.EditTransform(session->entity) = driven;
}

VehicleControls ReadVehicleControls(const InputState& input, bool keyboardCaptured, float deltaSeconds, float& keyboardSteering,
                                    bool manualGearbox, VehicleGearButtons* gearButtonsHeld)
{
    VehicleControls controls;
    controls.manualGearbox = manualGearbox;
    VehicleGearButtons gearButtons;
    float steeringTarget = 0.0f;
    if (!keyboardCaptured)
    {
        gearButtons.up = input.IsKeyDown(KeyCode(SDL_SCANCODE_E));
        gearButtons.down = input.IsKeyDown(KeyCode(SDL_SCANCODE_Q));
        controls.clutchPedal = manualGearbox && input.IsKeyDown(KeyCode(SDL_SCANCODE_N));
        controls.throttle += IsEitherKeyDown(input, SDL_SCANCODE_W, SDL_SCANCODE_UP) ? 1.0f : 0.0f;
        controls.throttle -= IsEitherKeyDown(input, SDL_SCANCODE_S, SDL_SCANCODE_DOWN) ? 1.0f : 0.0f;
        steeringTarget += IsEitherKeyDown(input, SDL_SCANCODE_D, SDL_SCANCODE_RIGHT) ? 1.0f : 0.0f;
        steeringTarget -= IsEitherKeyDown(input, SDL_SCANCODE_A, SDL_SCANCODE_LEFT) ? 1.0f : 0.0f;
        controls.handBrake = input.IsKeyDown(KeyCode(SDL_SCANCODE_SPACE)) ? 1.0f : 0.0f;
    }
    const bool centring = steeringTarget == 0.0f || steeringTarget * keyboardSteering < 0.0f;
    const float seconds = centring ? kKeyboardCentreSeconds : kKeyboardSteerSeconds;
    keyboardSteering = MoveTowards(keyboardSteering, steeringTarget, std::max(deltaSeconds, 0.0f) / seconds);
    controls.steering = keyboardSteering;

    const int gamepadIndex = input.GetFirstConnectedGamepadIndex();
    if (!keyboardCaptured && gamepadIndex >= 0)
    {
        const uint32_t player = static_cast<uint32_t>(gamepadIndex);
        controls.throttle += input.GetGamepadAxis(GamepadAxis::RightTrigger, player) - input.GetGamepadAxis(GamepadAxis::LeftTrigger, player);
        controls.steering += ApplyDeadZone(input.GetGamepadAxis(GamepadAxis::LeftX, player), kGamepadStickDeadZone);
        if (input.IsGamepadButtonDown(GamepadButton::East, player))
        {
            controls.handBrake = 1.0f;
        }
        gearButtons.up = gearButtons.up || input.IsGamepadButtonDown(GamepadButton::RightShoulder, player);
        gearButtons.down = gearButtons.down || input.IsGamepadButtonDown(GamepadButton::LeftShoulder, player);
        controls.clutchPedal = controls.clutchPedal || (manualGearbox && input.IsGamepadButtonDown(GamepadButton::South, player));
    }

    if (gearButtonsHeld != nullptr)
    {
        if (manualGearbox)
        {
            controls.gearShifts = (gearButtons.up && !gearButtonsHeld->up ? 1 : 0) - (gearButtons.down && !gearButtonsHeld->down ? 1 : 0);
        }
        *gearButtonsHeld = gearButtons;
    }

    controls.throttle = std::clamp(controls.throttle, -1.0f, 1.0f);
    controls.steering = std::clamp(controls.steering, -1.0f, 1.0f);
    return controls;
}

float AddSceneCollision(PhysicsWorld& physics, const RendererWorld& renderWorld, const ISceneWorld& scene, entt::entity exclude, float fallbackFloor)
{
    bool any = false;
    float lowest = fallbackFloor;
    const auto noteLowest = [&](float meshLowest)
    {
        lowest = any ? std::min(lowest, meshLowest) : meshLowest;
        any = true;
    };
    std::vector<glm::vec3> worldVertices;

    // A model that carries the game's own collision (an Assetto Corsa track's physics meshes) collides
    // through that alone, each surface at its friction: what it draws is for looking at.
    std::unordered_set<entt::entity> collidesByItself;
    for (const entt::entity entity : scene.Registry().view<const ModelComponent>())
    {
        if (entity == exclude)
        {
            continue;
        }
        const std::shared_ptr<const LoadedModelData> model = ModelCache::Get(scene.GetModel(entity).sourcePath);
        if (!model || model->collisionMeshes.empty())
        {
            continue;
        }
        collidesByItself.insert(entity);
        const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
        size_t triangles = 0;
        for (const ModelCollisionMesh& mesh : model->collisionMeshes)
        {
            worldVertices.clear();
            worldVertices.reserve(mesh.positions.size());
            float meshLowest = 0.0f;
            for (const glm::vec3& position : mesh.positions)
            {
                const glm::vec3 world = glm::vec3(modelMatrix * glm::vec4(position, 1.0f));
                meshLowest = worldVertices.empty() ? world.y : std::min(meshLowest, world.y);
                worldVertices.push_back(world);
            }
            SurfaceGrip grip;
            grip.friction = mesh.friction;
            grip.frictionCap = mesh.frictionCap;
            grip.wetSpeedFalloff = mesh.wetSpeedFalloff;
            grip.slidingShare = mesh.slidingShare;
            grip.rollingResistance = mesh.rollingResistance;
            if (physics.AddStaticMesh(worldVertices, mesh.indices, grip))
            {
                noteLowest(meshLowest);
                triangles += mesh.indices.size() / 3;
            }
        }
        LOG_INFO(
            "'{}' collides through its own {} surfaces ({} triangles); its drawn meshes are left out",
            scene.GetTag(entity).name,
            model->collisionMeshes.size(),
            triangles);
    }

    // Water surfaces, from every model that has any (the drawn surface of a sea or lake).
    for (const entt::entity entity : scene.Registry().view<const ModelComponent>())
    {
        if (entity == exclude)
        {
            continue;
        }
        const std::shared_ptr<const LoadedModelData> model = ModelCache::Get(scene.GetModel(entity).sourcePath);
        if (!model || model->water.indices.empty())
        {
            continue;
        }
        const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
        worldVertices.clear();
        worldVertices.reserve(model->water.positions.size());
        for (const glm::vec3& position : model->water.positions)
        {
            worldVertices.push_back(glm::vec3(modelMatrix * glm::vec4(position, 1.0f)));
        }
        physics.AddWaterSurface(worldVertices, model->water.indices);
        LOG_INFO("'{}' has {} water triangles: cars float and sink below them", scene.GetTag(entity).name, model->water.indices.size() / 3);
    }

    size_t groundCoverSubmeshes = 0;
    size_t groundCoverTriangles = 0;
    for (const std::shared_ptr<const CpuRenderSubmesh>& entry : renderWorld.GetRenderSubmeshes())
    {
        const CpuRenderSubmesh& submesh = *entry;
        // Glass, smoke, decals and the top of water are drawn over surfaces rather than being any; alpha-tested
        // fences and foliage still count. A skinned mesh moves (a character, a driver in the car).
        if (submesh.entity == exclude || !submesh.mesh || !submesh.mesh->IsValid() || submesh.decal || submesh.water || submesh.skinned ||
            submesh.alphaMode == MaterialAlphaMode::Blend || !scene.IsValidEntity(submesh.entity) ||
            collidesByItself.count(submesh.entity) != 0 || scene.Registry().all_of<StreamedComponent>(submesh.entity))
        {
            continue;
        }

        const glm::mat4 modelMatrix = scene.GetModelMatrix(submesh.entity);
        worldVertices.clear();
        worldVertices.reserve(submesh.mesh->vertices.size());
        float submeshLowest = 0.0f;
        for (const Vertex& vertex : submesh.mesh->vertices)
        {
            const glm::vec3 position = glm::vec3(modelMatrix * glm::vec4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f));
            submeshLowest = worldVertices.empty() ? position.y : std::min(submeshLowest, position.y);
            worldVertices.push_back(position);
        }
        // Grass and weeds are cut-out cards, drawn and not solid: the verge is a million of them.
        if (submesh.alphaMode == MaterialAlphaMode::Mask && IsGroundCover(worldVertices, submesh.mesh->indices))
        {
            ++groundCoverSubmeshes;
            groundCoverTriangles += submesh.mesh->indices.size() / 3;
            continue;
        }
        if (physics.AddStaticMesh(worldVertices, submesh.mesh->indices))
        {
            noteLowest(submeshLowest);
        }
    }
    if (groundCoverSubmeshes > 0)
    {
        LOG_INFO("Left {} ground-cover submeshes ({} triangles of grass and weeds) out of the collision", groundCoverSubmeshes, groundCoverTriangles);
    }
    return lowest;
}

float SteeringWheelTurn(
    const VehicleWheelAnimation& animation,
    const PhysicsPose& body,
    const std::vector<VehicleWheelState>& wheels,
    const glm::quat& vehicleToModel,
    const glm::vec3& scale)
{
    // The front wheels' steering to the right, in radians: a right turn is a negative turn about Y.
    float rightSteer = 0.0f;
    for (size_t index = 0; index < 2 && index < wheels.size(); ++index)
    {
        const VehicleWheelMotion motion = ComputeVehicleWheelMotion(body, wheels[index].pose, vehicleToModel, scale);
        // The turn about Y is twice the half-angle atan2(y, w); q and -q are one rotation.
        const float sign = motion.steer.w < 0.0f ? -1.0f : 1.0f;
        rightSteer -= std::atan2(motion.steer.y * sign, motion.steer.w * sign);
    }
    return rightSteer / glm::radians(animation.maxSteerDegrees) * glm::radians(animation.steeringWheelLockDegrees);
}

std::vector<glm::mat4> BuildWheelSubmeshTransforms(
    const VehicleWheelAnimation& animation,
    const PhysicsPose& body,
    const std::vector<VehicleWheelState>& wheels,
    const glm::quat& vehicleToModel,
    const glm::vec3& scale)
{
    // Per corner: the transform of its wheel, its brake disc and its suspension, each moving the
    // vertices from where the model has them at rest to where the simulation has the wheel.
    struct CornerTransforms
    {
        glm::mat4 wheel{1.0f};
        glm::mat4 disc{1.0f};
        glm::mat4 suspension{1.0f};
    };
    const LoadedModelData& model = *animation.model;
    const std::array<glm::vec3, kModelWheelCornerCount>& restCenters = animation.restCenters;
    std::array<CornerTransforms, kModelWheelCornerCount> corners;
    for (size_t index = 0; index < kModelWheelCornerCount && index < wheels.size(); ++index)
    {
        const VehicleWheelMotion motion = ComputeVehicleWheelMotion(body, wheels[index].pose, vehicleToModel, scale);
        const glm::mat4 toCenter = glm::translate(glm::mat4(1.0f), motion.center);
        const glm::mat4 fromRest = glm::translate(glm::mat4(1.0f), -restCenters[index]);
        corners[index].wheel = toCenter * glm::mat4_cast(motion.steer * motion.spin) * fromRest;
        corners[index].disc = toCenter * glm::mat4_cast(motion.steer) * fromRest;
        corners[index].suspension = glm::translate(glm::mat4(1.0f), motion.center - restCenters[index]);
    }

    glm::mat4 steeringWheel(1.0f);
    if (model.steeringWheel.has_value())
    {
        const float turn = SteeringWheelTurn(animation, body, wheels, vehicleToModel, scale);
        steeringWheel = glm::translate(glm::mat4(1.0f), model.steeringWheel->center) *
                        glm::rotate(glm::mat4(1.0f), turn, model.steeringWheel->axis) *
                        glm::translate(glm::mat4(1.0f), -model.steeringWheel->center);
    }

    std::vector<glm::mat4> transforms(model.submeshes.size(), glm::mat4(1.0f));
    for (size_t index = 0; index < model.submeshes.size(); ++index)
    {
        const ModelSubmeshData& submesh = model.submeshes[index];
        if (submesh.steeringWheel)
        {
            transforms[index] = steeringWheel;
            continue;
        }
        if (submesh.wheelCorner >= kModelWheelCornerCount)
        {
            continue;
        }
        switch (submesh.wheelPart)
        {
        case ModelWheelPart::Wheel:
            transforms[index] = corners[submesh.wheelCorner].wheel;
            break;
        case ModelWheelPart::Disc:
            transforms[index] = corners[submesh.wheelCorner].disc;
            break;
        case ModelWheelPart::Suspension:
            transforms[index] = corners[submesh.wheelCorner].suspension;
            break;
        case ModelWheelPart::None:
            break;
        }
    }
    return transforms;
}

void UpdateCameraOrbit(VehicleCameraOrbit& orbit, bool lookHeld, float yawDeltaDegrees, float pitchDeltaDegrees, float recenterRate, float deltaSeconds)
{
    if (lookHeld)
    {
        orbit.yawDegrees = std::remainder(orbit.yawDegrees + yawDeltaDegrees, 360.0f);
        orbit.pitchDegrees = std::clamp(orbit.pitchDegrees + pitchDeltaDegrees, -kOrbitMaxLowerDegrees, kOrbitMaxRaiseDegrees);
        return;
    }
    const float keep = std::exp(-std::max(recenterRate, 0.0f) * std::max(deltaSeconds, 0.0f));
    orbit.yawDegrees *= keep;
    orbit.pitchDegrees *= keep;
}

void UpdateChaseCamera(
    Camera& camera,
    const PhysicsPose& vehiclePose,
    const VehicleCameraSettings& settings,
    float deltaSeconds,
    const VehicleCameraOrbit& orbit)
{
    // Behind the car along its heading on the ground, so it neither pitches nor rolls with the body.
    glm::vec3 heading = vehiclePose.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    heading.y = 0.0f;
    if (glm::length(heading) < 1e-3f)
    {
        heading = camera.GetForward();
        heading.y = 0.0f;
        if (glm::length(heading) < 1e-3f)
        {
            heading = glm::vec3(0.0f, 0.0f, 1.0f);
        }
    }
    heading = glm::normalize(heading);

    const glm::vec3 target = vehiclePose.position + glm::vec3(0.0f, settings.lookHeight, 0.0f);
    // The place behind the car and above it, swung about the point the camera looks at: pitched about
    // the car's right, then turned about the vertical.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 lookAt = vehiclePose.position + glm::vec3(0.0f, settings.lookHeight, 0.0f);
    glm::vec3 arm = vehiclePose.position + glm::vec3(0.0f, settings.height, 0.0f) - heading * settings.distance - lookAt;
    arm = glm::angleAxis(glm::radians(orbit.pitchDegrees), glm::normalize(glm::cross(up, heading))) * arm;
    arm = glm::angleAxis(glm::radians(orbit.yawDegrees), up) * arm;
    // Locked to the car, with no smoothing of the car's travel or of its heading: the camera keeps the
    // same place relative to the body, as in Gran Turismo 7, so a view from the car's side stays on the
    // side instead of lagging round as the heading changes.
    camera.position = lookAt + arm;

    const glm::vec3 toTarget = target - camera.position;
    if (glm::length(toTarget) > 1e-4f)
    {
        const glm::vec3 direction = glm::normalize(toTarget);
        camera.yawDegrees = glm::degrees(std::atan2(direction.z, direction.x));
        camera.pitchDegrees = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    }
}

std::array<VehicleCameraMount, kVehicleMountedViewCount> ComputeCameraMounts(
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    const std::optional<VehicleCameraMount>& eyes,
    const std::optional<glm::vec3>& steeringWheel,
    const std::function<std::optional<float>(float z)>& heightAt)
{
    const glm::vec3 size = glm::max(boundsMax - boundsMin, glm::vec3(0.1f));
    const float centreX = (boundsMin.x + boundsMax.x) * 0.5f;

    VehicleCameraMount cockpit;
    if (eyes.has_value())
    {
        cockpit = *eyes;
    }
    else if (steeringWheel.has_value())
    {
        cockpit.position = *steeringWheel + glm::vec3(0.0f, kEyesOverSteeringWheel, -kEyesBehindSteeringWheel);
        cockpit.pitchDegrees = kCockpitPitchDegrees;
    }
    else
    {
        cockpit.position = glm::vec3(
            centreX,
            boundsMin.y + size.y * kEyesHeightShare,
            (boundsMin.z + boundsMax.z) * 0.5f - size.z * kEyesBehindMiddleShare);
        cockpit.pitchDegrees = kCockpitPitchDegrees;
    }

    // Over the bonnet towards the windscreen, a little above the body there, so the bonnet shows at the
    // bottom of the view. Never above the eyes: a profile that caught the roof would put it there.
    VehicleCameraMount bonnet;
    bonnet.position.x = centreX;
    bonnet.position.z = cockpit.position.z + (boundsMax.z - cockpit.position.z) * kBonnetShareToFront;
    const std::optional<float> body = heightAt ? heightAt(bonnet.position.z) : std::nullopt;
    bonnet.position.y = std::min(body.has_value() ? *body + kBonnetCameraLift : cockpit.position.y - kBonnetCameraLift, cockpit.position.y);
    bonnet.pitchDegrees = kBonnetPitchDegrees;

    // Low, just ahead of the nose: the car is out of the view.
    VehicleCameraMount bumper;
    bumper.position = glm::vec3(
        centreX,
        boundsMin.y + std::clamp(size.y * kBumperHeightShare, kBumperMinHeight, kBumperMaxHeight),
        boundsMax.z + kBumperCameraAhead);
    bumper.pitchDegrees = 0.0f;

    return {cockpit, bonnet, bumper};
}

void UpdateMountedCamera(Camera& camera, const PhysicsPose& vehiclePose, const VehicleCameraMount& mount, const VehicleCameraOrbit& look)
{
    // In vehicle space (+Z forward, +X the car's left): the head turned about the body's up, then nodded.
    const float yaw = glm::radians(look.yawDegrees);
    const float pitch = glm::radians(std::clamp(mount.pitchDegrees - look.pitchDegrees, -89.0f, 89.0f));
    const glm::vec3 forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const glm::vec3 up(-std::sin(yaw) * std::sin(pitch), std::cos(pitch), -std::cos(yaw) * std::sin(pitch));

    // Fixed to the body with no smoothing, so it pitches and rolls with it as GT7's cockpit view does.
    camera.position = vehiclePose.position + vehiclePose.rotation * mount.position;
    camera.worldUp = glm::normalize(vehiclePose.rotation * up);
    const glm::vec3 direction = glm::normalize(vehiclePose.rotation * forward);
    camera.yawDegrees = glm::degrees(std::atan2(direction.z, direction.x));
    camera.pitchDegrees = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
}
}
}
