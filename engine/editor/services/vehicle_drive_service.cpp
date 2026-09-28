#include "vehicle_drive_service.h"

#include <engine/core/input/input.h>
#include <engine/core/log/log.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/logic/world_bounds.h>
#include <engine/renderer/renderer_world.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
// Keyboard steering eases to full lock in this many seconds, and back to centre faster.
constexpr float kKeyboardSteerSeconds = 0.35f;
constexpr float kKeyboardCentreSeconds = 0.2f;
constexpr float kGamepadStickDeadZone = 0.12f;
// The ground plane under everything, so a car driven off the edge of the track lands somewhere.
constexpr float kGroundPlaneHalfSize = 5000.0f;
constexpr float kGroundPlaneHalfThickness = 0.5f;

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
    session->vehicleToModel = VehicleToModelRotation(tuning.modelFront);
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
    const VehicleSettings settings = FitVehicleSettingsToBounds(glm::min(cornerA, cornerB), glm::max(cornerA, cornerB), tuning);

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
    LOG_INFO(
        "Driving '{}': {} static bodies, {} collision triangles, built in {:.0f} ms",
        session->name,
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
        state.vehicleDrive.session.reset();
        return false;
    }

    const VehicleControls controls = ReadVehicleControls(state.input, keyboardCaptured, deltaSeconds, session->keyboardSteering);
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
        session->physics->Update(deltaSeconds);
    }
    else if (session->stepRequested)
    {
        session->physics->Update(PhysicsWorld::kFixedStepSeconds);
    }
    session->stepRequested = false;

    const PhysicsPose pose = session->physics->GetVehiclePose(session->vehicle);
    PhysicsPose modelPose = pose;
    modelPose.rotation = pose.rotation * session->vehicleToModel;
    world.ApplyTransformMatrix(session->entity, ComposeMatrix(modelPose, session->scale));
    if (state.vehicleDrive.camera.follow)
    {
        UpdateChaseCamera(state.camera, pose, state.vehicleDrive.camera, deltaSeconds);
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
        status.telemetry = session->physics->GetVehicleTelemetry(session->vehicle);
        status.staticBodyCount = session->physics->GetStaticBodyCount();
        status.staticTriangleCount = session->physics->GetStaticTriangleCount();
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

VehicleControls ReadVehicleControls(const InputState& input, bool keyboardCaptured, float deltaSeconds, float& keyboardSteering)
{
    VehicleControls controls;
    float steeringTarget = 0.0f;
    if (!keyboardCaptured)
    {
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
        if (input.IsGamepadButtonDown(GamepadButton::South, player))
        {
            controls.handBrake = 1.0f;
        }
    }

    controls.throttle = std::clamp(controls.throttle, -1.0f, 1.0f);
    controls.steering = std::clamp(controls.steering, -1.0f, 1.0f);
    return controls;
}

float AddSceneCollision(PhysicsWorld& physics, const RendererWorld& renderWorld, const ISceneWorld& scene, entt::entity exclude, float fallbackFloor)
{
    bool any = false;
    float lowest = fallbackFloor;
    std::vector<glm::vec3> worldVertices;
    for (const CpuRenderSubmesh& submesh : renderWorld.GetRenderSubmeshes())
    {
        // Glass, smoke and decals are drawn over surfaces rather than being any; alpha-tested
        // fences and foliage still count.
        if (submesh.entity == exclude || !submesh.mesh || !submesh.mesh->IsValid() || submesh.decal ||
            submesh.alphaMode == MaterialAlphaMode::Blend || !scene.IsValidEntity(submesh.entity))
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
        if (physics.AddStaticMesh(worldVertices, submesh.mesh->indices))
        {
            lowest = any ? std::min(lowest, submeshLowest) : submeshLowest;
            any = true;
        }
    }
    return lowest;
}

void UpdateChaseCamera(Camera& camera, const PhysicsPose& vehiclePose, const VehicleCameraSettings& settings, float deltaSeconds)
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
    const glm::vec3 desired = vehiclePose.position - heading * settings.distance + glm::vec3(0.0f, settings.height, 0.0f);
    const float follow = deltaSeconds <= 0.0f ? 1.0f : 1.0f - std::exp(-std::max(settings.stiffness, 0.0f) * deltaSeconds);
    camera.position = glm::mix(camera.position, desired, follow);

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
