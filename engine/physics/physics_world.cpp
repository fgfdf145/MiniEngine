#include "physics_world.h"

// Jolt.h comes first: it sets up the configuration every other Jolt header depends on.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>

namespace me
{

namespace
{
namespace ObjectLayers
{
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::uint kCount = 2;
}

namespace BroadPhaseLayers
{
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
}

// A wheel rolls on a surface whose normal is within 70 degrees of up. Steeper is a wall: the chassis
// hits it, but a wheel's cylinder cast would take its face for ground and climb it.
constexpr float kMinWheelSurfaceNormalY = 0.34f;
constexpr uint64_t kWheelsIgnoreBody = 1; // JPH::Body::GetUserData of a wall

// What a vehicle's wheels collide with: everything but the vehicle itself and the walls.
class WheelBodyFilter final : public JPH::BodyFilter
{
  public:
    explicit WheelBodyFilter(const JPH::BodyID& vehicle) : m_vehicle(vehicle)
    {
    }

    bool ShouldCollide(const JPH::BodyID& body) const override
    {
        return body != m_vehicle;
    }

    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return body.GetUserData() != kWheelsIgnoreBody;
    }

  private:
    JPH::BodyID m_vehicle;
};

constexpr JPH::uint kMaxBodies = 65536;
constexpr JPH::uint kBodyMutexCount = 0; // Jolt's default
constexpr JPH::uint kMaxBodyPairs = 65536;
constexpr JPH::uint kMaxContactConstraints = 10240;
constexpr size_t kTempAllocatorBytes = 16 * 1024 * 1024;

void JoltTrace(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    char message[1024];
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[Jolt] %s\n", message);
}

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
{
    std::fprintf(stderr, "[Jolt] %s:%u: (%s) %s\n", file, line, expression, message != nullptr ? message : "");
    return true; // break into the debugger
}
#endif

// Jolt's allocator, factory and type registry are process-wide; the last world to go takes them down.
std::mutex g_joltMutex;
int g_joltUsers = 0;

void AcquireJolt()
{
    const std::lock_guard lock(g_joltMutex);
    if (g_joltUsers++ == 0)
    {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void ReleaseJolt()
{
    const std::lock_guard lock(g_joltMutex);
    if (--g_joltUsers == 0)
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

JPH::Vec3 ToJolt(const glm::vec3& value)
{
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 ToJoltPosition(const glm::vec3& value)
{
    return JPH::RVec3(value.x, value.y, value.z);
}

JPH::Quat ToJolt(const glm::quat& value)
{
    return JPH::Quat(value.x, value.y, value.z, value.w).Normalized();
}

glm::vec3 FromJolt(JPH::Vec3Arg value)
{
    return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
}

#ifdef JPH_DOUBLE_PRECISION
glm::vec3 FromJolt(JPH::RVec3Arg value)
{
    return glm::vec3(static_cast<float>(value.GetX()), static_cast<float>(value.GetY()), static_cast<float>(value.GetZ()));
}
#endif

glm::quat FromJolt(JPH::QuatArg value)
{
    return glm::quat(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
}

PhysicsPose FromJolt(JPH::RMat44Arg transform)
{
    PhysicsPose pose;
    pose.position = FromJolt(transform.GetTranslation());
    pose.rotation = FromJolt(transform.GetQuaternion());
    return pose;
}

PhysicsPose Interpolate(const PhysicsPose& from, const PhysicsPose& to, float alpha)
{
    PhysicsPose pose;
    pose.position = glm::mix(from.position, to.position, alpha);
    pose.rotation = glm::slerp(from.rotation, to.rotation, alpha);
    return pose;
}

// The pose of a vehicle and its wheels after one fixed step.
struct VehicleSnapshot
{
    PhysicsPose chassis;
    std::vector<VehicleWheelState> wheels;
};

bool HasTyreGrip(const VehicleTyreSettings& tyres)
{
    return tyres.longitudinalGrip > 0.0f || tyres.lateralGrip > 0.0f;
}

// A wheel's friction curves from its axle's tyres: rising from nothing to the peak at the slip that
// gives it, then down to the share of the peak the physics engine's own curves fall to, at the
// same multiple of that slip.
void ApplyTyres(JPH::WheelSettingsWV& wheel, const VehicleTyreSettings& tyres)
{
    // The physics engine's default: 1.2 at a slip ratio of 0.06 and 3 degrees, 1.0 at 0.2 and 20.
    constexpr float kDefaultPostPeakShare = 1.0f / 1.2f;
    constexpr float kSlipRatioFall = 0.2f / 0.06f;
    constexpr float kSlipAngleFall = 20.0f / 3.0f;
    const float postPeakShare = tyres.postPeakShare > 0.0f ? std::min(tyres.postPeakShare, 1.0f) : kDefaultPostPeakShare;
    if (tyres.longitudinalGrip > 0.0f)
    {
        const float peak = tyres.peakSlipRatio > 0.0f ? tyres.peakSlipRatio : 0.06f;
        wheel.mLongitudinalFriction.Clear();
        wheel.mLongitudinalFriction.AddPoint(0.0f, 0.0f);
        wheel.mLongitudinalFriction.AddPoint(peak, tyres.longitudinalGrip);
        wheel.mLongitudinalFriction.AddPoint(peak * kSlipRatioFall, tyres.longitudinalGrip * postPeakShare);
    }
    if (tyres.lateralGrip > 0.0f)
    {
        const float peak = tyres.peakSlipAngleDegrees > 0.0f ? tyres.peakSlipAngleDegrees : 3.0f;
        wheel.mLateralFriction.Clear();
        wheel.mLateralFriction.AddPoint(0.0f, 0.0f);
        wheel.mLateralFriction.AddPoint(peak, tyres.lateralGrip);
        wheel.mLateralFriction.AddPoint(peak * kSlipAngleFall, tyres.lateralGrip * postPeakShare);
    }
    if (tyres.inertia > 0.0f)
    {
        wheel.mInertia = tyres.inertia;
    }
}

JPH::Ref<JPH::VehicleConstraintSettings> BuildVehicleConstraintSettings(const VehicleSettings& settings)
{
    JPH::Ref<JPH::VehicleConstraintSettings> vehicle = new JPH::VehicleConstraintSettings();
    vehicle->mUp = JPH::Vec3::sAxisY();
    vehicle->mForward = JPH::Vec3::sAxisZ();
    vehicle->mMaxPitchRollAngle = JPH::DegreesToRadians(std::clamp(settings.maxPitchRollDegrees, 0.0f, 180.0f));

    // Front left, front right, rear left, rear right; +X is the car's left.
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const bool front = index < 2;
        const VehicleWheelGeometry mount = GetVehicleWheelMount(settings, index);
        JPH::WheelSettingsWV* wheel = new JPH::WheelSettingsWV();
        wheel->mPosition = ToJolt(mount.center);
        wheel->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
        wheel->mSteeringAxis = JPH::Vec3::sAxisY();
        wheel->mWheelUp = JPH::Vec3::sAxisY();
        wheel->mWheelForward = JPH::Vec3::sAxisZ();
        wheel->mSuspensionMinLength = std::max(settings.suspensionMinLength, 0.0f);
        wheel->mSuspensionMaxLength = std::max(settings.suspensionMaxLength, wheel->mSuspensionMinLength);
        // A fixed stiffness for a quarter of the car's weight. The frequency mode would scale the
        // spring by the body's effective mass at the wheel, which its inertia makes 10 to 30% off a
        // quarter, and the car would not settle at the ride height ComputeRestSuspensionLength gives.
        const float quarterMass = std::max(settings.massKg, 1.0f) * 0.25f;
        const float omega = 2.0f * std::numbers::pi_v<float> * std::max(settings.suspensionFrequencyHz, 0.01f);
        wheel->mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
        wheel->mSuspensionSpring.mStiffness = quarterMass * omega * omega;
        wheel->mSuspensionSpring.mDamping = 2.0f * std::max(settings.suspensionDamping, 0.0f) * quarterMass * omega;
        wheel->mRadius = std::max(mount.radius, 0.01f);
        wheel->mWidth = std::max(mount.width, 0.01f);
        wheel->mMaxSteerAngle = front ? JPH::DegreesToRadians(std::clamp(settings.maxSteerAngleDegrees, 0.0f, 89.0f)) : 0.0f;
        // The axles share the four wheels' total by the front's share, so an even 0.5 leaves each
        // wheel at the brake torque.
        const float axleShare = std::clamp(front ? settings.frontBrakeShare : 1.0f - settings.frontBrakeShare, 0.0f, 1.0f);
        ApplyTyres(*wheel, front ? settings.frontTyres : settings.rearTyres);
        wheel->mMaxBrakeTorque = ComputeBrakeTorquePerWheel(settings) * 2.0f * axleShare;
        wheel->mMaxHandBrakeTorque = front ? 0.0f : std::max(settings.maxHandBrakeTorque, 0.0f);
        vehicle->mWheels.push_back(wheel);
    }

    if (settings.antiRollBars)
    {
        vehicle->mAntiRollBars.resize(2);
        vehicle->mAntiRollBars[0].mLeftWheel = 0;
        vehicle->mAntiRollBars[0].mRightWheel = 1;
        vehicle->mAntiRollBars[1].mLeftWheel = 2;
        vehicle->mAntiRollBars[1].mRightWheel = 3;
    }

    JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings();
    controller->mEngine.mMaxTorque = std::max(settings.maxEngineTorque, 0.0f);
    controller->mEngine.mMinRPM = std::max(settings.minRpm, 1.0f);
    controller->mEngine.mMaxRPM = std::max(settings.maxRpm, controller->mEngine.mMinRPM + 1.0f);
    if (settings.engineInertia > 0.0f)
    {
        controller->mEngine.mInertia = settings.engineInertia;
    }
    if (settings.torqueCurve.size() >= 2 && settings.maxEngineTorque > 0.0f)
    {
        // The physics reads the curve at the engine's rpm over its maximum.
        controller->mEngine.mNormalizedTorque.Clear();
        controller->mEngine.mNormalizedTorque.Reserve(static_cast<JPH::uint>(settings.torqueCurve.size()));
        for (const glm::vec2& point : settings.torqueCurve)
        {
            controller->mEngine.mNormalizedTorque.AddPoint(
                point.x / controller->mEngine.mMaxRPM, std::clamp(point.y / settings.maxEngineTorque, 0.0f, 1.0f));
        }
        controller->mEngine.mNormalizedTorque.Sort();
    }

    if (!settings.gearRatios.empty())
    {
        controller->mTransmission.mGearRatios.assign(settings.gearRatios.begin(), settings.gearRatios.end());
        if (settings.reverseGearRatio != 0.0f)
        {
            controller->mTransmission.mReverseGearRatios = {settings.reverseGearRatio};
        }
    }
    if (settings.clutchStrength > 0.0f)
    {
        controller->mTransmission.mClutchStrength = settings.clutchStrength;
    }
    if (settings.gearSwitchSeconds > 0.0f)
    {
        controller->mTransmission.mSwitchTime = settings.gearSwitchSeconds;
    }
    if (settings.clutchReleaseSeconds > 0.0f)
    {
        controller->mTransmission.mClutchReleaseTime = settings.clutchReleaseSeconds;
    }
    if (settings.shiftUpRpm > 0.0f)
    {
        controller->mTransmission.mShiftUpRPM = settings.shiftUpRpm;
    }
    if (settings.shiftDownRpm > 0.0f)
    {
        controller->mTransmission.mShiftDownRPM = settings.shiftDownRpm;
    }

    const float limitedSlipRatio = settings.limitedSlipDifferentials ? 1.4f : FLT_MAX;
    auto addDifferential = [&](int leftWheel, int rightWheel, float torqueRatio)
    {
        JPH::VehicleDifferentialSettings differential;
        differential.mLeftWheel = leftWheel;
        differential.mRightWheel = rightWheel;
        differential.mEngineTorqueRatio = torqueRatio;
        // A limited-slip differential is a clutch pack that the vehicle applies each step
        // (CoupleDifferentialWheels); the physics engine's own would snap all the torque to one wheel.
        differential.mLimitedSlipRatio = FLT_MAX;
        if (settings.finalDriveRatio > 0.0f)
        {
            differential.mDifferentialRatio = settings.finalDriveRatio;
        }
        controller->mDifferentials.push_back(differential);
    };
    switch (settings.drive)
    {
    case VehicleDrive::FrontWheel:
        addDifferential(0, 1, 1.0f);
        break;
    case VehicleDrive::AllWheel:
        addDifferential(0, 1, 0.5f);
        addDifferential(2, 3, 0.5f);
        break;
    case VehicleDrive::RearWheel:
    default:
        addDifferential(2, 3, 1.0f);
        break;
    }
    controller->mDifferentialLimitedSlipRatio = limitedSlipRatio;
    vehicle->mController = controller;
    return vehicle;
}
}

struct PhysicsWorld::Impl
{
    struct Vehicle
    {
        JPH::Body* body = nullptr;
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTester> collisionTester;
        std::unique_ptr<WheelBodyFilter> wheelFilter;
        VehicleControls controls;
        // The air acting on the car: where, and how much drag and downforce per square metre of dynamic pressure.
        std::vector<VehicleAeroSurface> aeroSurfaces;
        float direction = 1.0f; // the gearbox's drive or reverse, see ResolveVehicleDriverInput
        // The brakes: the wheels' settings whose torque is set each step, the torque of each wheel as the
        // fixed front/rear split has it, and (when dynamicBrakeBias) the loads that share the total.
        std::vector<JPH::WheelSettingsWV*> wheelSettings;
        std::array<float, kVehicleWheelCount> staticBrakeTorque{};
        std::array<float, kVehicleWheelCount> filteredLoad{};
        bool dynamicBrakeBias = false;
        bool filteredLoadValid = false;
        // Traction control, see LimitClutchTorque.
        float tractionControlGrip = 0.0f;
        VehicleDrive drive = VehicleDrive::RearWheel;
        // The share of the drive torque a limited-slip differential can move between the wheels, see
        // CoupleDifferentialWheels; 0 for an open one.
        float limitedSlipLock = 0.0f;
        // The clutch's own strength, which traction control lowers while it slips it.
        float defaultClutchStrength = 10.0f;
        float weightNewtons = 0.0f;
        VehicleSnapshot previous;
        VehicleSnapshot current;
    };

    // Sets each wheel's brake torque for the coming step. Fixed, it is the front/rear split's. Dynamic, the
    // four wheels share the same total by the load they carried in the last step, a little smoothed, so a
    // wheel that weight has left brakes less and one it has moved to brakes more, all wheels reaching
    // their grip together.
    void DistributeBrakeTorque(Vehicle& vehicle) const
    {
        const size_t count = std::min(vehicle.wheelSettings.size(), vehicle.staticBrakeTorque.size());
        const auto useFixedSplit = [&]
        {
            for (size_t index = 0; index < count; ++index)
            {
                vehicle.wheelSettings[index]->mMaxBrakeTorque = vehicle.staticBrakeTorque[index];
            }
        };
        if (!vehicle.dynamicBrakeBias || vehicle.current.wheels.size() < count)
        {
            useFixedSplit();
            return;
        }

        // The loads over about three steps: the solver's force on a wheel jumps about from step to step.
        constexpr float kLoadFilterSeconds = 0.05f;
        // No wheel takes more than half the total, so one wheel on the ground does not carry the whole
        // car's brakes.
        constexpr float kMaxShareOfTotal = 0.5f;
        const float blend = vehicle.filteredLoadValid ? 1.0f - std::exp(-kFixedStepSeconds / kLoadFilterSeconds) : 1.0f;
        float total = 0.0f;
        float loadSum = 0.0f;
        for (size_t index = 0; index < count; ++index)
        {
            const VehicleWheelState& wheel = vehicle.current.wheels[index];
            const float load = wheel.inContact ? std::max(wheel.suspensionForce, 0.0f) : 0.0f;
            vehicle.filteredLoad[index] += (load - vehicle.filteredLoad[index]) * blend;
            loadSum += vehicle.filteredLoad[index];
            total += vehicle.staticBrakeTorque[index];
        }
        vehicle.filteredLoadValid = true;

        // With hardly any weight on the ground (just dropped, or launched off a jump) the loads say nothing.
        if (loadSum < vehicle.weightNewtons * 0.1f)
        {
            useFixedSplit();
            return;
        }
        for (size_t index = 0; index < count; ++index)
        {
            vehicle.wheelSettings[index]->mMaxBrakeTorque = total * std::min(vehicle.filteredLoad[index] / loadSum, kMaxShareOfTotal);
        }
    }

    // Traction control by the clutch: it slips once the engine asks the driven wheels for more torque than their
    // tyres can hold (their peak grip on the load they carry, times the setting), so that the wheels get no more
    // than they can pass to the ground while the engine keeps its revs and its throttle, as a driver slips the
    // clutch on a start. The clutch's torque is its strength times the gap between the engine's speed and the
    // wheels', so the strength is set to the torque allowed over the gap, up to its own.
    void LimitClutchTorque(Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        JPH::VehicleTransmission& transmission = controller.GetTransmission();
        const float ratio = std::abs(transmission.GetCurrentRatio());
        if (vehicle.tractionControlGrip <= 0.0f || ratio < 1e-3f || controller.GetDifferentials().empty() ||
            vehicle.current.wheels.size() < kVehicleWheelCount)
        {
            transmission.mClutchStrength = vehicle.defaultClutchStrength;
            return;
        }
        float holdTorque = 0.0f;
        float wheelSpeed = 0.0f;
        int driven = 0;
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            const bool front = index < 2;
            const bool isDriven = vehicle.drive == VehicleDrive::AllWheel || (vehicle.drive == VehicleDrive::FrontWheel) == front;
            const VehicleWheelState& wheel = vehicle.current.wheels[index];
            if (!isDriven)
            {
                continue;
            }
            ++driven;
            wheelSpeed += std::abs(wheel.angularVelocity);
            if (wheel.inContact)
            {
                holdTorque += wheel.longitudinalPeakFriction * std::max(wheel.suspensionForce, 0.0f) * wheel.radius;
            }
        }
        const float overall = ratio * controller.GetDifferentials()[0].mDifferentialRatio;
        const float engineSide = holdTorque * vehicle.tractionControlGrip / overall;
        const float engineSpeed = controller.GetEngine().GetCurrentRPM() * 2.0f * std::numbers::pi_v<float> / 60.0f;
        const float gap = std::abs(engineSpeed - wheelSpeed / static_cast<float>(std::max(driven, 1)) * overall);
        transmission.mClutchStrength = std::clamp(engineSide / std::max(gap, 1.0f), 0.05f, vehicle.defaultClutchStrength);
    }

    // A limited-slip differential as a clutch pack between the two wheels of an axle: a torque, growing with
    // how much faster one turns than the other and held to a limit that follows the drive torque, is taken
    // from the faster wheel and given to the slower, so that the pair is kept turning together and the drive
    // goes where the grip is. The physics engine's own gives all the torque to the slower wheel once the gap
    // passes a ratio: that wheel then spins up past the other and they take turns, each spinning several times
    // the ground's speed in turn.
    void CoupleDifferentialWheels(Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        if (vehicle.limitedSlipLock <= 0.0f)
        {
            return;
        }
        constexpr float kPreloadTorque = 40.0f; // Nm, holds them together even when coasting
        const float driveTorque = controller.GetEngine().GetTorque(1.0f) * std::abs(controller.GetTransmission().GetCurrentRatio());
        for (const JPH::VehicleDifferentialSettings& differential : controller.GetDifferentials())
        {
            if (differential.mLeftWheel < 0 || differential.mRightWheel < 0)
            {
                continue;
            }
            auto* left = static_cast<JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(differential.mLeftWheel)]);
            auto* right = static_cast<JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(differential.mRightWheel)]);
            const float limit = kPreloadTorque + vehicle.limitedSlipLock * driveTorque * differential.mDifferentialRatio * differential.mEngineTorqueRatio * 0.5f;
            // Stiff enough that a wheel is pulled to the other's speed within a few steps, and no stiffer than
            // the step can integrate.
            const float inertia = 0.5f * (left->GetSettings()->mInertia + right->GetSettings()->mInertia);
            const float stiffness = 0.25f * inertia / kFixedStepSeconds;
            const float torque = std::clamp(stiffness * (left->GetAngularVelocity() - right->GetAngularVelocity()), -limit, limit);
            left->ApplyTorque(-torque, kFixedStepSeconds);
            right->ApplyTorque(torque, kFixedStepSeconds);
        }
    }

    // Drag against the car's velocity and downforce along its down, on each surface where it sits.
    void ApplyAerodynamics(const Vehicle& vehicle)
    {
        if (vehicle.aeroSurfaces.empty())
        {
            return;
        }
        const JPH::Vec3 velocity = vehicle.body->GetLinearVelocity();
        const float speed = velocity.Length();
        if (speed < 0.1f)
        {
            return;
        }
        constexpr float kAirDensity = 1.2f; // kg/m^3, as the game uses
        const float dynamicPressure = 0.5f * kAirDensity * speed * speed;
        const JPH::Quat rotation = vehicle.body->GetRotation();
        const JPH::Vec3 down = rotation * JPH::Vec3(0.0f, -1.0f, 0.0f);
        JPH::BodyInterface& bodies = physicsSystem.GetBodyInterface();
        for (const VehicleAeroSurface& surface : vehicle.aeroSurfaces)
        {
            const JPH::Vec3 force = -velocity / speed * (dynamicPressure * surface.dragArea) + down * (dynamicPressure * surface.downforceArea);
            const JPH::RVec3 point = vehicle.body->GetCenterOfMassPosition() + rotation * ToJolt(surface.position);
            bodies.AddForce(vehicle.body->GetID(), force, point);
        }
    }

    Impl()
        : tempAllocator(kTempAllocatorBytes),
          jobSystem(
              JPH::cMaxPhysicsJobs,
              JPH::cMaxPhysicsBarriers,
              static_cast<int>(std::max(1u, std::thread::hardware_concurrency()) - 1))
    {
        // The filter table reads the layer mappings when it is built, so they go in first.
        broadPhaseLayers = std::make_unique<JPH::BroadPhaseLayerInterfaceTable>(ObjectLayers::kCount, BroadPhaseLayers::kCount);
        broadPhaseLayers->MapObjectToBroadPhaseLayer(ObjectLayers::kStatic, BroadPhaseLayers::kStatic);
        broadPhaseLayers->MapObjectToBroadPhaseLayer(ObjectLayers::kMoving, BroadPhaseLayers::kMoving);
        objectLayerPairs = std::make_unique<JPH::ObjectLayerPairFilterTable>(ObjectLayers::kCount);
        objectLayerPairs->EnableCollision(ObjectLayers::kMoving, ObjectLayers::kStatic);
        objectLayerPairs->EnableCollision(ObjectLayers::kMoving, ObjectLayers::kMoving);
        objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
            *broadPhaseLayers, BroadPhaseLayers::kCount, *objectLayerPairs, ObjectLayers::kCount);

        physicsSystem.Init(
            kMaxBodies,
            kBodyMutexCount,
            kMaxBodyPairs,
            kMaxContactConstraints,
            *broadPhaseLayers,
            *objectVsBroadPhase,
            *objectLayerPairs);
        physicsSystem.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    }

    ~Impl()
    {
        JPH::BodyInterface& bodies = physicsSystem.GetBodyInterface();
        for (Vehicle& vehicle : vehicles)
        {
            physicsSystem.RemoveStepListener(vehicle.constraint);
            physicsSystem.RemoveConstraint(vehicle.constraint);
            bodies.RemoveBody(vehicle.body->GetID());
            bodies.DestroyBody(vehicle.body->GetID());
        }
        for (const JPH::BodyID& body : staticBodies)
        {
            bodies.RemoveBody(body);
            bodies.DestroyBody(body);
        }
    }

    Vehicle& GetVehicle(VehicleId id)
    {
        if (id >= vehicles.size())
        {
            throw std::out_of_range("PhysicsWorld: unknown vehicle " + std::to_string(id));
        }
        return vehicles[id];
    }

    const Vehicle& GetVehicle(VehicleId id) const
    {
        return const_cast<Impl*>(this)->GetVehicle(id);
    }

    VehicleSnapshot Capture(const Vehicle& vehicle) const
    {
        VehicleSnapshot snapshot;
        snapshot.chassis.position = FromJolt(vehicle.body->GetPosition());
        snapshot.chassis.rotation = FromJolt(vehicle.body->GetRotation());
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        snapshot.wheels.reserve(wheels.size());
        for (JPH::uint index = 0; index < wheels.size(); ++index)
        {
            const JPH::Wheel& wheel = *wheels[index];
            VehicleWheelState state;
            // The wheel model's right is -X, the car's right; its up is +Y.
            state.pose = FromJolt(vehicle.constraint->GetWheelWorldTransform(index, -JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY()));
            // The roll is kept apart from the pose (GetVehicleWheels puts it back), for it is a turn about
            // the axle by an angle that the pose alone cannot follow past half a turn per step.
            constexpr float kTurn = 2.0f * std::numbers::pi_v<float>;
            state.spinAngle = wheel.GetRotationAngle();
            if (vehicle.previous.wheels.size() > index)
            {
                // The change in the physics engine's angle, folded to a half turn either way and then
                // moved by whole turns to the one nearest what the wheel's speed says it rolled.
                float step = state.spinAngle - vehicle.previous.wheels[index].spinAngle;
                step -= kTurn * std::round(step / kTurn);
                const float expected = wheel.GetAngularVelocity() * kFixedStepSeconds;
                step += kTurn * std::round((expected - step) / kTurn);
                state.spinStep = step;
            }
            state.pose.rotation = state.pose.rotation * glm::angleAxis(-state.spinAngle, glm::vec3(1.0f, 0.0f, 0.0f));
            state.inContact = wheel.HasContact();
            state.suspensionLength = wheel.GetSuspensionLength();
            state.angularVelocity = wheel.GetAngularVelocity();

            const JPH::WheelSettings& wheelSettings = *wheel.GetSettings();
            const JPH::Quat bodyRotation = vehicle.body->GetRotation();
            state.mount = FromJolt(vehicle.body->GetPosition() + bodyRotation * wheelSettings.mPosition);
            state.suspensionAxis = FromJolt(bodyRotation * wheelSettings.mSuspensionDirection.Normalized());
            state.suspensionMinLength = wheelSettings.mSuspensionMinLength;
            state.suspensionMaxLength = wheelSettings.mSuspensionMaxLength;
            state.radius = wheelSettings.mRadius;
            state.width = wheelSettings.mWidth;
            state.brakeTorque = static_cast<const JPH::WheelWV&>(wheel).GetSettings()->mMaxBrakeTorque;
            if (state.inContact)
            {
                const auto& wheelWV = static_cast<const JPH::WheelWV&>(wheel);
                // The solver's impulses over the step it ran are the step's mean forces.
                state.contactPosition = FromJolt(wheel.GetContactPosition());
                state.contactNormal = FromJolt(wheel.GetContactNormal());
                state.contactLongitudinal = FromJolt(wheel.GetContactLongitudinal());
                state.contactLateral = FromJolt(wheel.GetContactLateral());
                state.suspensionForce = wheel.GetSuspensionLambda() / kFixedStepSeconds;
                state.longitudinalForce = wheel.GetLongitudinalLambda() / kFixedStepSeconds;
                state.lateralForce = wheel.GetLateralLambda() / kFixedStepSeconds;
                state.slipRatio = wheelWV.mLongitudinalSlip;
                state.slipAngleDegrees = JPH::RadiansToDegrees(wheelWV.mLateralSlip);
                state.longitudinalFriction = wheelWV.mCombinedLongitudinalFriction;
                state.lateralFriction = wheelWV.mCombinedLateralFriction;
                // The ground's share is what the combined value is of the curve's at this slip; where
                // that is nothing (no slip) the ground counts for what it does by default.
                const auto peakOf = [](const JPH::LinearCurve& curve, float slip, float combined)
                {
                    float peak = 0.0f;
                    for (const JPH::LinearCurve::Point& point : curve.mPoints)
                    {
                        peak = std::max(peak, point.mY);
                    }
                    const float atSlip = curve.GetValue(std::abs(slip));
                    return atSlip > 1e-3f ? peak * combined / atSlip : peak;
                };
                const JPH::WheelSettingsWV& tyre = *wheelWV.GetSettings();
                state.longitudinalPeakFriction = peakOf(tyre.mLongitudinalFriction, wheelWV.mLongitudinalSlip, wheelWV.mCombinedLongitudinalFriction);
                state.lateralPeakFriction = peakOf(tyre.mLateralFriction, JPH::RadiansToDegrees(wheelWV.mLateralSlip), wheelWV.mCombinedLateralFriction);
            }
            snapshot.wheels.push_back(state);
        }
        return snapshot;
    }

    float Alpha() const
    {
        return std::clamp(accumulatedSeconds / kFixedStepSeconds, 0.0f, 1.0f);
    }

    // Declared before the physics system, so they outlive it.
    JPH::TempAllocatorImpl tempAllocator;
    JPH::JobSystemThreadPool jobSystem;
    std::unique_ptr<JPH::BroadPhaseLayerInterfaceTable> broadPhaseLayers;
    std::unique_ptr<JPH::ObjectLayerPairFilterTable> objectLayerPairs;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    JPH::PhysicsSystem physicsSystem;

    std::vector<JPH::BodyID> staticBodies;
    size_t staticTriangleCount = 0;
    std::vector<Vehicle> vehicles;
    float accumulatedSeconds = 0.0f;
    bool broadPhaseDirty = false;
};

PhysicsWorld::PhysicsWorld()
{
    AcquireJolt();
    try
    {
        m_impl = std::make_unique<Impl>();
    }
    catch (...)
    {
        ReleaseJolt();
        throw;
    }
}

PhysicsWorld::~PhysicsWorld()
{
    m_impl.reset();
    ReleaseJolt();
}

bool PhysicsWorld::AddStaticMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices, float friction)
{
    JPH::VertexList joltVertices;
    joltVertices.reserve(vertices.size());
    for (const glm::vec3& vertex : vertices)
    {
        joltVertices.push_back(JPH::Float3(vertex.x, vertex.y, vertex.z));
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t index = 0; index + 2 < indices.size(); index += 3)
    {
        const uint32_t a = indices[index];
        const uint32_t b = indices[index + 1];
        const uint32_t c = indices[index + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size() || a == b || b == c || a == c)
        {
            continue;
        }
        triangles.push_back(JPH::IndexedTriangle(a, b, c));
    }
    if (triangles.empty())
    {
        return false;
    }

    // The car's wheels roll on the walkable triangles; the steep ones (walls) only stop the chassis.
    JPH::IndexedTriangleList walkable;
    JPH::IndexedTriangleList steep;
    for (const JPH::IndexedTriangle& triangle : triangles)
    {
        const JPH::Vec3 a(joltVertices[triangle.mIdx[0]]);
        const JPH::Vec3 b(joltVertices[triangle.mIdx[1]]);
        const JPH::Vec3 c(joltVertices[triangle.mIdx[2]]);
        const JPH::Vec3 normal = (b - a).Cross(c - a);
        const float length = normal.Length();
        if (!(length > 1e-12f))
        {
            continue; // degenerate
        }
        (normal.GetY() >= kMinWheelSurfaceNormalY * length ? walkable : steep).push_back(triangle);
    }

    const float bodyFriction = std::max(friction, 0.0f);
    const auto addBody = [&](JPH::IndexedTriangleList list, uint64_t userData) -> size_t
    {
        if (list.empty())
        {
            return 0;
        }
        const JPH::MeshShapeSettings shapeSettings(joltVertices, std::move(list));
        const size_t triangleCount = shapeSettings.mIndexedTriangles.size();
        const JPH::ShapeSettings::ShapeResult shape = shapeSettings.Create();
        if (shape.HasError() || triangleCount == 0)
        {
            return 0;
        }
        JPH::BodyCreationSettings bodySettings(
            shape.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ObjectLayers::kStatic);
        bodySettings.mFriction = bodyFriction;
        bodySettings.mUserData = userData;
        const JPH::BodyID body = m_impl->physicsSystem.GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
        if (body.IsInvalid())
        {
            throw std::runtime_error("PhysicsWorld: out of bodies for static geometry");
        }
        m_impl->staticBodies.push_back(body);
        m_impl->staticTriangleCount += triangleCount;
        m_impl->broadPhaseDirty = true;
        return triangleCount;
    };
    return addBody(std::move(walkable), 0) + addBody(std::move(steep), kWheelsIgnoreBody) > 0;
}

void PhysicsWorld::AddStaticBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, float friction)
{
    const JPH::Vec3 extents = JPH::Vec3::sMax(ToJolt(halfExtents), JPH::Vec3::sReplicate(0.01f));
    JPH::BodyCreationSettings bodySettings(
        new JPH::BoxShape(extents), ToJoltPosition(center), ToJolt(rotation), JPH::EMotionType::Static, ObjectLayers::kStatic);
    bodySettings.mFriction = std::max(friction, 0.0f);
    const JPH::BodyID body = m_impl->physicsSystem.GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    if (body.IsInvalid())
    {
        throw std::runtime_error("PhysicsWorld: out of bodies for static geometry");
    }
    m_impl->staticBodies.push_back(body);
    m_impl->broadPhaseDirty = true;
}

size_t PhysicsWorld::GetStaticBodyCount() const
{
    return m_impl->staticBodies.size();
}

size_t PhysicsWorld::GetStaticTriangleCount() const
{
    return m_impl->staticTriangleCount;
}

VehicleId PhysicsWorld::AddVehicle(const VehicleSettings& settings, const PhysicsPose& pose)
{
    Impl& impl = *m_impl;

    // The box sits where the settings put it in vehicle space; the centre of mass is moved from the
    // box's centre by the offset.
    const JPH::Vec3 halfExtents = JPH::Vec3::sMax(ToJolt(settings.chassisHalfExtents), JPH::Vec3::sReplicate(0.05f));
    const JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(halfExtents, std::min(0.05f, halfExtents.ReduceMin() * 0.5f));
    const JPH::RotatedTranslatedShapeSettings placedBox(ToJolt(settings.chassisCenter), JPH::Quat::sIdentity(), box);
    const JPH::OffsetCenterOfMassShapeSettings chassisShape(ToJolt(settings.centerOfMassOffset), placedBox.Create().Get());
    const JPH::ShapeSettings::ShapeResult shape = chassisShape.Create();
    if (shape.HasError())
    {
        throw std::runtime_error(std::string("PhysicsWorld: failed to build the vehicle's chassis: ") + shape.GetError().c_str());
    }

    JPH::BodyCreationSettings bodySettings(
        shape.Get(), ToJoltPosition(pose.position), ToJolt(pose.rotation), JPH::EMotionType::Dynamic, ObjectLayers::kMoving);
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = std::max(settings.massKg, 1.0f);
    bodySettings.mLinearDamping = std::max(settings.linearDamping, 0.0f);
    // A fast car against a thin wall would otherwise pass through it between two steps.
    bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;

    Impl::Vehicle vehicle;
    vehicle.body = impl.physicsSystem.GetBodyInterface().CreateBody(bodySettings);
    if (vehicle.body == nullptr)
    {
        throw std::runtime_error("PhysicsWorld: out of bodies for the vehicle");
    }
    impl.physicsSystem.GetBodyInterface().AddBody(vehicle.body->GetID(), JPH::EActivation::Activate);

    vehicle.aeroSurfaces = settings.aeroSurfaces;
    vehicle.constraint = new JPH::VehicleConstraint(*vehicle.body, *BuildVehicleConstraintSettings(settings));
    if (HasTyreGrip(settings.frontTyres) || HasTyreGrip(settings.rearTyres))
    {
        // Tyre grip as the game measures it is multiplied by the surface's, not averaged with it.
        vehicle.constraint->SetCombineFriction(
            [](JPH::uint, float& ioLongitudinal, float& ioLateral, const JPH::Body& body, const JPH::SubShapeID&)
            {
                ioLongitudinal *= body.GetFriction();
                ioLateral *= body.GetFriction();
            });
    }
    // Casting the wheels' cylinders rolls them over kerbs and seams a ray would catch on.
    vehicle.collisionTester = new JPH::VehicleCollisionTesterCastCylinder(ObjectLayers::kMoving);
    vehicle.wheelFilter = std::make_unique<WheelBodyFilter>(vehicle.body->GetID());
    vehicle.collisionTester->SetBodyFilter(vehicle.wheelFilter.get());
    vehicle.constraint->SetVehicleCollisionTester(vehicle.collisionTester);
    impl.physicsSystem.AddConstraint(vehicle.constraint);
    impl.physicsSystem.AddStepListener(vehicle.constraint);

    // The wheels' settings were made above and are ours to change: the controller reads the brake torque
    // from them each step.
    for (size_t index = 0; index < kVehicleWheelCount && index < vehicle.constraint->GetWheels().size(); ++index)
    {
        auto* wheelSettings = const_cast<JPH::WheelSettingsWV*>(static_cast<const JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(index)])->GetSettings());
        vehicle.wheelSettings.push_back(wheelSettings);
        vehicle.staticBrakeTorque[index] = wheelSettings->mMaxBrakeTorque;
    }
    vehicle.dynamicBrakeBias = settings.dynamicBrakeBias;
    vehicle.tractionControlGrip = std::max(settings.tractionControlGrip, 0.0f);
    vehicle.defaultClutchStrength = settings.clutchStrength > 0.0f ? settings.clutchStrength : 10.0f;
    vehicle.limitedSlipLock = settings.limitedSlipDifferentials ? std::max(settings.limitedSlipLock, 0.0f) : 0.0f;
    vehicle.drive = settings.drive;
    vehicle.weightNewtons = std::max(settings.massKg, 1.0f) * 9.81f;

    vehicle.current = impl.Capture(vehicle);
    vehicle.previous = vehicle.current;
    impl.vehicles.push_back(std::move(vehicle));
    return static_cast<VehicleId>(impl.vehicles.size() - 1);
}

void PhysicsWorld::SetVehicleControls(VehicleId id, const VehicleControls& controls)
{
    m_impl->GetVehicle(id).controls = controls;
}

void PhysicsWorld::ResetVehicle(VehicleId id, const PhysicsPose& pose)
{
    Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    JPH::BodyInterface& bodies = m_impl->physicsSystem.GetBodyInterface();
    const JPH::BodyID body = vehicle.body->GetID();
    bodies.SetPositionAndRotation(body, ToJoltPosition(pose.position), ToJolt(pose.rotation), JPH::EActivation::Activate);
    bodies.SetLinearAndAngularVelocity(body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    for (JPH::Wheel* wheel : vehicle.constraint->GetWheels())
    {
        wheel->SetAngularVelocity(0.0f);
    }
    auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
    controller->GetEngine().SetCurrentRPM(controller->GetEngine().mMinRPM);
    controller->SetDriverInput(0.0f, 0.0f, 0.0f, 0.0f);
    vehicle.controls = {};
    vehicle.direction = 1.0f;
    vehicle.filteredLoadValid = false;
    vehicle.current = m_impl->Capture(vehicle);
    // Nothing has rolled: the capture compared the wheels with the step before the reset.
    for (VehicleWheelState& wheel : vehicle.current.wheels)
    {
        wheel.spinStep = 0.0f;
    }
    vehicle.previous = vehicle.current;
}

PhysicsPose PhysicsWorld::GetVehiclePose(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    return Interpolate(vehicle.previous.chassis, vehicle.current.chassis, m_impl->Alpha());
}

std::vector<VehicleWheelState> PhysicsWorld::GetVehicleWheels(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    const float alpha = m_impl->Alpha();
    std::vector<VehicleWheelState> wheels = vehicle.current.wheels;
    for (size_t index = 0; index < wheels.size() && index < vehicle.previous.wheels.size(); ++index)
    {
        const VehicleWheelState& before = vehicle.previous.wheels[index];
        wheels[index].pose = Interpolate(before.pose, wheels[index].pose, alpha);
        // The roll runs on from where it was before the step by the step's share, straight through
        // however many turns that is.
        wheels[index].spinAngle -= (1.0f - alpha) * wheels[index].spinStep;
        wheels[index].pose.rotation = wheels[index].pose.rotation * glm::angleAxis(wheels[index].spinAngle, glm::vec3(1.0f, 0.0f, 0.0f));
        // The points drawn on the wheel move with it; the forces are the last step's.
        wheels[index].mount = glm::mix(before.mount, wheels[index].mount, alpha);
        if (before.inContact && wheels[index].inContact)
        {
            wheels[index].contactPosition = glm::mix(before.contactPosition, wheels[index].contactPosition, alpha);
        }
    }
    return wheels;
}

VehicleTelemetry PhysicsWorld::GetVehicleTelemetry(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    const auto* controller = static_cast<const JPH::WheeledVehicleController*>(vehicle.constraint->GetController());

    VehicleTelemetry telemetry;
    const JPH::Vec3 localVelocity = vehicle.body->GetRotation().Conjugated() * vehicle.body->GetLinearVelocity();
    telemetry.forwardSpeed = localVelocity.GetZ();
    telemetry.engineRpm = controller->GetEngine().GetCurrentRPM();
    telemetry.gear = controller->GetTransmission().GetCurrentGear();
    for (const JPH::Wheel* wheel : vehicle.constraint->GetWheels())
    {
        telemetry.wheelsInContact += wheel->HasContact() ? 1u : 0u;
    }
    return telemetry;
}

int PhysicsWorld::Update(float deltaSeconds)
{
    Impl& impl = *m_impl;
    if (impl.broadPhaseDirty)
    {
        // Static geometry added one body at a time leaves the broad phase tree unbalanced.
        impl.physicsSystem.OptimizeBroadPhase();
        impl.broadPhaseDirty = false;
    }

    impl.accumulatedSeconds += std::max(deltaSeconds, 0.0f);
    int steps = 0;
    while (impl.accumulatedSeconds >= kFixedStepSeconds && steps < kMaxStepsPerUpdate)
    {
        JPH::BodyInterface& bodies = impl.physicsSystem.GetBodyInterface();
        for (Impl::Vehicle& vehicle : impl.vehicles)
        {
            const JPH::Vec3 localVelocity = vehicle.body->GetRotation().Conjugated() * vehicle.body->GetLinearVelocity();
            const VehicleDriverInput input = ResolveVehicleDriverInput(vehicle.controls, localVelocity.GetZ(), vehicle.direction);
            impl.ApplyAerodynamics(vehicle);
            impl.DistributeBrakeTorque(vehicle);
            if (input.forward != 0.0f || input.right != 0.0f || input.brake != 0.0f || input.handBrake != 0.0f)
            {
                // A car that came to rest is asleep and would ignore the driver.
                bodies.ActivateBody(vehicle.body->GetID());
            }
            auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
            impl.CoupleDifferentialWheels(vehicle, *controller);
            impl.LimitClutchTorque(vehicle, *controller);
            controller->SetDriverInput(input.forward, input.right, input.brake, input.handBrake);
        }

        const JPH::EPhysicsUpdateError error =
            impl.physicsSystem.Update(kFixedStepSeconds, 1, &impl.tempAllocator, &impl.jobSystem);
        if (error != JPH::EPhysicsUpdateError::None)
        {
            JoltTrace("PhysicsSystem::Update reported error flags 0x%x", static_cast<unsigned>(error));
        }

        for (Impl::Vehicle& vehicle : impl.vehicles)
        {
            vehicle.previous = std::move(vehicle.current);
            vehicle.current = impl.Capture(vehicle);
        }
        impl.accumulatedSeconds -= kFixedStepSeconds;
        ++steps;
    }
    if (steps == kMaxStepsPerUpdate)
    {
        impl.accumulatedSeconds = std::min(impl.accumulatedSeconds, kFixedStepSeconds);
    }
    return steps;
}
}
