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
#include <memory>
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
// The right stick swings the chase camera round the car this fast at full deflection.
constexpr float kGamepadOrbitDegreesPerSecond = 180.0f;
// The ground plane under everything, so a car driven off the edge of the track lands somewhere.
constexpr float kGroundPlaneHalfSize = 5000.0f;
constexpr float kGroundPlaneHalfThickness = 0.5f;
// How far the right mouse button may tilt the chase camera down onto the car and up from below.
constexpr float kOrbitMaxRaiseDegrees = 60.0f;
constexpr float kOrbitMaxLowerDegrees = 10.0f;

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

// Vehicle space is +Z forward; a model facing -Z is half a turn about Y from it (its own inverse).
glm::quat VehicleToModelRotation(VehicleModelFront front)
{
    return front == VehicleModelFront::NegativeZ ? glm::quat(0.0f, 0.0f, 1.0f, 0.0f) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// The way a model faces, from where its wheels are: the front wheels' Z against the rear's.
VehicleModelFront ModelFrontFromWheels(const ModelWheelRig& rig)
{
    const float frontZ = (rig.corners[0].center.z + rig.corners[1].center.z) * 0.5f;
    const float rearZ = (rig.corners[2].center.z + rig.corners[3].center.z) * 0.5f;
    return frontZ < rearZ ? VehicleModelFront::NegativeZ : VehicleModelFront::PositiveZ;
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
}

namespace VehicleDriveService
{

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

    session->engineMinRpm = settings.minRpm;
    session->engineMaxRpm = settings.maxRpm;

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
        UpdateChaseCamera(state.camera, session->startPose, state.vehicleDrive.camera, 0.0f);
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
        session->haptics = VehicleHapticsState{};
    }
}

void SetPaused(RendererSharedState& state, bool paused)
{
    if (VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        session->paused = paused;
    }
}

void Step(RendererSharedState& state)
{
    if (VehicleDriveSession* session = state.vehicleDrive.session.get(); session != nullptr && session->paused)
    {
        session->stepRequested = true;
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
        return;
    }

    const uint32_t player = static_cast<uint32_t>(gamepadIndex);
    if (session.paused || keyboardCaptured || SDL_GetKeyboardFocus() == nullptr)
    {
        input.SetGamepadFeedback(player, GamepadFeedback{});
        return;
    }

    VehicleHapticsInput haptics;
    haptics.telemetry = session.physics->GetVehicleTelemetry(session.vehicle);
    haptics.minRpm = session.engineMinRpm;
    haptics.maxRpm = session.engineMaxRpm;
    haptics.rightTrigger = input.GetGamepadAxis(GamepadAxis::RightTrigger, player);
    haptics.leftTrigger = input.GetGamepadAxis(GamepadAxis::LeftTrigger, player);
    haptics.adaptiveTriggers = input.GetGamepadType(player) == SDL_GAMEPAD_TYPE_PS5;
    input.SetGamepadFeedback(player, ComputeVehicleFeedback(state.vehicleDrive.haptics, haptics, session.haptics, deltaSeconds));
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
        state.rendererWorld.ClearSubmeshLocalTransforms(session->entity);
        state.input.ClearGamepadFeedback();
        state.vehicleDrive.session.reset();
        return false;
    }

    const VehicleControls controls = ReadVehicleControls(
        state.input, keyboardCaptured, deltaSeconds, session->keyboardSteering, state.vehicleDrive.manualGearbox, &session->gearButtonsHeld);
    const bool resetDown =
        (!keyboardCaptured && state.input.IsKeyDown(KeyCode(SDL_SCANCODE_BACKSPACE))) ||
        (state.input.GetFirstConnectedGamepadIndex() >= 0 && !keyboardCaptured &&
         state.input.IsGamepadButtonDown(GamepadButton::Back, static_cast<uint32_t>(state.input.GetFirstConnectedGamepadIndex())));
    if (resetDown && !session->resetHeld)
    {
        Reset(state);
    }
    session->resetHeld = resetDown;

    session->physics->SetVehicleControls(session->vehicle, controls);
    if (!session->paused)
    {
        // At most this long on physics a frame, so a world too slow for real time slows down rather
        // than the frame rate.
        constexpr float kPhysicsBudgetSeconds = 0.025f;
        const int steps = session->physics->Update(deltaSeconds, kPhysicsBudgetSeconds);
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

    const PhysicsPose pose =session->physics->GetVehiclePose(session->vehicle);
    PhysicsPose modelPose = pose;
    modelPose.rotation = pose.rotation * session->vehicleToModel;
    world.ApplyTransformMatrix(session->entity, ComposeMatrix(modelPose, session->scale));
    if (session->wheels.has_value())
    {
        state.rendererWorld.SetSubmeshLocalTransforms(
            session->entity,
            BuildWheelSubmeshTransforms(
                *session->wheels,
                pose,
                session->physics->GetVehicleWheels(session->vehicle),
                session->vehicleToModel,
                session->scale));
    }
    if (state.vehicleDrive.camera.follow)
    {
        // Holding the right mouse button looks around the car: dragging right swings the camera to its left side, as if turning the view to the right.
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
        UpdateCameraOrbit(session->orbit, lookHeld, lookYaw, lookPitch, state.vehicleDrive.camera, deltaSeconds);
        UpdateChaseCamera(state.camera, pose, state.vehicleDrive.camera, deltaSeconds, session->orbit);
    }
    return true;
}

VehicleDriveStatus GetStatus(const RendererSharedState& state)
{
    VehicleDriveStatus status;
    status.lastError = state.vehicleDrive.lastError;
    if (const VehicleDriveSession* session = state.vehicleDrive.session.get())
    {
        status.active = true;
        status.paused = session->paused;
        status.vehicleName = session->name;
        status.carData = session->carData;
        status.telemetry = session->physics->GetVehicleTelemetry(session->vehicle);
        status.wheels = session->physics->GetVehicleWheels(session->vehicle);
        status.linkage = session->physics->GetVehicleLinkage(session->vehicle);
        status.staticBodyCount = session->physics->GetStaticBodyCount();
        status.staticTriangleCount = session->physics->GetStaticTriangleCount();
        status.realTimeShare = session->realTimeShare;
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
        gearButtons.neutral = input.IsKeyDown(KeyCode(SDL_SCANCODE_N));
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
        gearButtons.neutral = gearButtons.neutral || input.IsGamepadButtonDown(GamepadButton::South, player);
    }

    if (gearButtonsHeld != nullptr)
    {
        if (manualGearbox)
        {
            controls.gearShifts = (gearButtons.up && !gearButtonsHeld->up ? 1 : 0) - (gearButtons.down && !gearButtonsHeld->down ? 1 : 0);
            controls.selectNeutral = gearButtons.neutral && !gearButtonsHeld->neutral;
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

    size_t groundCoverSubmeshes = 0;
    size_t groundCoverTriangles = 0;
    for (const CpuRenderSubmesh& submesh : renderWorld.GetRenderSubmeshes())
    {
        // Glass, smoke and decals are drawn over surfaces rather than being any; alpha-tested
        // fences and foliage still count.
        if (submesh.entity == exclude || !submesh.mesh || !submesh.mesh->IsValid() || submesh.decal ||
            submesh.alphaMode == MaterialAlphaMode::Blend || !scene.IsValidEntity(submesh.entity) ||
            collidesByItself.count(submesh.entity) != 0)
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
    // The front wheels' steering to the right, in radians: a right turn is a negative turn about Y.
    float rightSteer = 0.0f;
    for (size_t index = 0; index < kModelWheelCornerCount && index < wheels.size(); ++index)
    {
        const VehicleWheelMotion motion = ComputeVehicleWheelMotion(body, wheels[index].pose, vehicleToModel, scale);
        const glm::mat4 toCenter = glm::translate(glm::mat4(1.0f), motion.center);
        const glm::mat4 fromRest = glm::translate(glm::mat4(1.0f), -restCenters[index]);
        corners[index].wheel = toCenter * glm::mat4_cast(motion.steer * motion.spin) * fromRest;
        corners[index].disc = toCenter * glm::mat4_cast(motion.steer) * fromRest;
        corners[index].suspension = glm::translate(glm::mat4(1.0f), motion.center - restCenters[index]);
        if (index < 2)
        {
            // The turn about Y is twice the half-angle atan2(y, w); q and -q are one rotation.
            const float sign = motion.steer.w < 0.0f ? -1.0f : 1.0f;
            rightSteer -= std::atan2(motion.steer.y * sign, motion.steer.w * sign);
        }
    }

    // The wheel turns clockwise for the driver, looking along its column, when the car steers right;
    // at full lock of the front wheels it has turned kSteeringWheelLockDegrees.
    glm::mat4 steeringWheel(1.0f);
    if (model.steeringWheel.has_value())
    {
        const float turn = rightSteer / glm::radians(animation.maxSteerDegrees) * glm::radians(animation.steeringWheelLockDegrees);
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

void UpdateCameraOrbit(VehicleCameraOrbit& orbit, bool lookHeld, float yawDeltaDegrees, float pitchDeltaDegrees, const VehicleCameraSettings& settings, float deltaSeconds)
{
    if (lookHeld)
    {
        orbit.yawDegrees = std::remainder(orbit.yawDegrees + yawDeltaDegrees, 360.0f);
        orbit.pitchDegrees = std::clamp(orbit.pitchDegrees + pitchDeltaDegrees, -kOrbitMaxLowerDegrees, kOrbitMaxRaiseDegrees);
        return;
    }
    const float keep = std::exp(-std::max(settings.lookRecenterRate, 0.0f) * std::max(deltaSeconds, 0.0f));
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
}
}
