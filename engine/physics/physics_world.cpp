#include "physics_world.h"
#include "vehicle_suspension.h"

#include <engine/suspension/suspension_corner.h>
#include <engine/tyre/tyre_brush.h>

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
#include <memory>
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

// With an unsprung mass the ground may come this much closer to the mount than the wheel's full
// bump: the tyre's own deflection.
constexpr float kTyreDeflectionRoom = 0.05f;

// The multibody suspension's straight-spring stand-in: the mount sits this far above the wheel's
// design centre, enough for the bump travel of either axle and the tyre's deflection under it.
float MultibodyDesignLength(const VehicleSettings& settings)
{
    const float bump = std::max(settings.frontSuspension.bumpStopTravel, settings.rearSuspension.bumpStopTravel);
    return std::max(bump, 0.03f) + 0.04f + 0.05f + kTyreDeflectionRoom;
}

// The axle's wheels are masses of their own on tyre springs (the data gives hub mass and tyre rate):
// see PhysicsWorld::Impl::UpdateCorners.
bool HasUnsprungMass(const VehicleSuspensionAxle& axle)
{
    return axle.hubMass > 0.0f && axle.tyreRate > 0.0f;
}

// The physics engine's spring carries the tyre's force, which the suspension sets each step through
// the preload; this stiffness only keeps the spring defined, small enough that the body's motion
// within a step hardly changes the force.
constexpr float kTyreCarrierStiffness = 100.0f;

// A hub moving slower than this (m/s) has settled.
constexpr double kHubSettledRate = 5e-4;

// Where the wheel's centre is at the design position: where the model draws it at rest.
glm::vec3 MultibodyDesignCenter(const VehicleSettings& settings, size_t index)
{
    return GetVehicleWheelMount(settings, index).center - glm::vec3(0.0f, ComputeRestSuspensionLength(settings, 9.81f), 0.0f);
}

// The load each axle's wheel carries standing still, from where the centre of mass sits between the axles.
float StaticWheelLoad(const VehicleSettings& settings, bool front)
{
    float frontShare = settings.frontWeightShare;
    if (!(frontShare > 0.0f && frontShare < 1.0f))
    {
        const float com = (settings.chassisCenter + settings.centerOfMassOffset).z;
        const float frontZ = GetVehicleWheelMount(settings, 0).center.z;
        const float rearZ = GetVehicleWheelMount(settings, 2).center.z;
        frontShare = std::abs(frontZ - rearZ) > 1e-3f ? std::clamp((com - rearZ) / (frontZ - rearZ), 0.05f, 0.95f) : 0.5f;
    }
    return 0.5f * std::max(settings.massKg, 1.0f) * 9.81f * (front ? frontShare : 1.0f - frontShare);
}

// A wheel's brush tyre from its axle's tyre figures: the peak grip at the load it carries standing
// still (a road tyre's 1.1 without data), the slip angle of the lateral peak (7 degrees without), the
// share of grip left well past the peak, the wheel's size and the axle's tyre rate.
tyre::BrushTyreParameters BuildBrushTyreParameters(const VehicleSettings& settings, size_t index)
{
    const bool front = index < 2;
    const VehicleTyreSettings& tyres = front ? settings.frontTyres : settings.rearTyres;
    const VehicleSuspensionAxle& axle = front ? settings.frontSuspension : settings.rearSuspension;
    const VehicleWheelGeometry mount = GetVehicleWheelMount(settings, index);
    double grip = 1.1;
    if (tyres.longitudinalGrip > 0.0f && tyres.lateralGrip > 0.0f)
    {
        // The bristles share one friction coefficient: the two directions' mean.
        grip = 0.5 * (tyres.longitudinalGrip + tyres.lateralGrip);
    }
    else if (tyres.lateralGrip > 0.0f || tyres.longitudinalGrip > 0.0f)
    {
        grip = std::max(tyres.lateralGrip, tyres.longitudinalGrip);
    }
    const double peakAngle = (tyres.peakSlipAngleDegrees > 0.0f ? tyres.peakSlipAngleDegrees : 7.0f) * std::numbers::pi / 180.0;
    const double falloff = tyres.postPeakShare > 0.0f ? tyres.postPeakShare : 0.85f;
    return tyre::MakeBrushTyreParameters(
        grip, StaticWheelLoad(settings, front), peakAngle, falloff, std::max(mount.radius, 0.05f), std::max(mount.width, 0.05f), axle.tyreRate);
}

JPH::Ref<JPH::VehicleConstraintSettings> BuildVehicleConstraintSettings(const VehicleSettings& settings)
{
    JPH::Ref<JPH::VehicleConstraintSettings> vehicle = new JPH::VehicleConstraintSettings();
    vehicle->mUp = JPH::Vec3::sAxisY();
    vehicle->mForward = JPH::Vec3::sAxisZ();
    vehicle->mMaxPitchRollAngle = JPH::DegreesToRadians(std::clamp(settings.maxPitchRollDegrees, 0.0f, 180.0f));

    // Front left, front right, rear left, rear right; +X is the car's left.
    const bool multibody = HasSuspensionGeometry(settings);
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
        if (multibody)
        {
            // The multibody suspension sets the spring each step and steers through the rack: the
            // straight spring starts at its design length, and the wheels' own steering is off.
            const VehicleSuspensionAxle& axle = front ? settings.frontSuspension : settings.rearSuspension;
            const float bump = std::max(axle.bumpStopTravel, 0.03f) + 0.04f;
            const float droop = axle.reboundStopTravel > 0.0f ? axle.reboundStopTravel : 0.08f;
            const float designLength = MultibodyDesignLength(settings);
            const glm::vec3 center = MultibodyDesignCenter(settings, index);
            wheel->mPosition = ToJolt(center + glm::vec3(0.0f, designLength, 0.0f));
            wheel->mSuspensionMinLength = designLength - bump;
            wheel->mSuspensionMaxLength = designLength + droop;
            wheel->mMaxSteerAngle = 0.0f;
            if (HasUnsprungMass(axle))
            {
                // The hub stops itself at full bump; the ground may come closer by the tyre's
                // deflection before the physics engine's hard stop.
                wheel->mSuspensionMinLength = std::max(designLength - bump - kTyreDeflectionRoom, 0.0f);
                wheel->mSuspensionSpring.mStiffness = kTyreCarrierStiffness;
                wheel->mSuspensionSpring.mDamping = 0.0f;
            }
        }
        vehicle->mWheels.push_back(wheel);
    }

    if (settings.antiRollBars && !multibody)
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

        // The multibody suspension, when the settings have one: a corner per wheel.
        struct Corner
        {
            glm::vec3 designCenter{0.0f}; // vehicle space
            float designLength = 0.0f;    // the straight spring's length at the design position
            double travel = 0.0;
            double antiRollBarRate = 0.0;
            float camberDegrees = 0.0f;
            float toeDegrees = 0.0f;
            float springForce = 0.0f;
            float antiRollBarForce = 0.0f;
            bool front = true;

            // With an unsprung mass the wheel's travel is a state of its own, and the physics engine's
            // spring is the tyre between the ground and the hub.
            bool unsprung = false;
            double hubMass = 0.0;
            double tyreRate = 0.0;
            double tyreDamping = 0.0;
            double travelRate = 0.0;
            double bumpTravel = 0.0;
            double droopTravel = 0.0;
            double designContactHeight = 0.0; // the unloaded tyre's lowest point at the design position
            double tyreDeflection = 0.0;
            glm::vec3 hubCenter{0.0f}; // vehicle space
            // Where the mount was when the physics engine last looked for the ground.
            JPH::RVec3 lastMount = JPH::RVec3::sZero();
            bool lastMountValid = false;
        };
        std::vector<Corner> corners;
        // Front and rear: each axle's two wheels (independent corners or a solid axle), stepped together.
        std::array<std::unique_ptr<suspension::AxleSuspension>, 2> axles;
        // A solid axle's propshaft torque reaction on its housing in the last step (N m, about the
        // car's length, positive lifting its left side), see ApplyAxleTorqueReaction.
        std::array<float, 2> axleTorqueReaction{};
        double rackAtLock = 0.0;
        // The body's velocities a step ago, for the acceleration the hubs ride on.
        JPH::Vec3 lastLinearVelocity = JPH::Vec3::sZero();
        JPH::Vec3 lastAngularVelocity = JPH::Vec3::sZero();
        bool lastVelocityValid = false;
        VehicleSettings settings;

        // The brush tyres (VehicleTyreModel::Brush), one per wheel, stepped inside the physics engine's
        // step once it has found the ground (ApplyBrushTyres), and what each did in the last step: its
        // output, the force it put on the body (world space) and the load it worked with.
        std::vector<tyre::BrushTyre> brushTyres;
        struct BrushWheel
        {
            tyre::BrushTyreOutput out;
            JPH::Vec3 force = JPH::Vec3::sZero();
            float load = 0.0f;
            bool contact = false;
        };
        std::array<BrushWheel, kVehicleWheelCount> brushWheels{};
    };

    void BuildCorners(Vehicle& vehicle) const
    {
        vehicle.corners.clear();
        const VehicleSettings& settings = vehicle.settings;
        if (!HasSuspensionGeometry(settings))
        {
            return;
        }
        // Each wheel's static load from where the centre of mass sits between the axles.
        const glm::vec3 com = settings.chassisCenter + settings.centerOfMassOffset;
        const float frontZ = MultibodyDesignCenter(settings, 0).z;
        const float rearZ = MultibodyDesignCenter(settings, 2).z;
        const float frontShare = std::abs(frontZ - rearZ) > 1e-3f ? std::clamp((com.z - rearZ) / (frontZ - rearZ), 0.05f, 0.95f) : 0.5f;
        const float weight = std::max(settings.massKg, 1.0f) * 9.81f;
        std::array<VehicleCornerSetup, kVehicleWheelCount> setups{};
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            const bool front = index < 2;
            const VehicleSuspensionAxle& axle = front ? settings.frontSuspension : settings.rearSuspension;
            const bool unsprung = HasUnsprungMass(axle);
            double load = 0.5 * weight * (front ? frontShare : 1.0f - frontShare);
            if (unsprung)
            {
                // The spring holds the body; the hub's own weight goes straight to the tyre.
                load -= axle.hubMass * 9.81;
            }
            setups[index] = BuildVehicleCorner(settings, index, load);
            const VehicleCornerSetup& setup = setups[index];
            Vehicle::Corner corner;
            corner.designCenter = MultibodyDesignCenter(settings, index);
            corner.designLength = MultibodyDesignLength(settings);
            corner.antiRollBarRate = setup.antiRollBarRate;
            corner.front = front;
            corner.unsprung = unsprung;
            corner.hubMass = axle.hubMass;
            corner.tyreRate = axle.tyreRate;
            corner.tyreDamping = std::max(axle.tyreDamping, 0.0f);
            corner.bumpTravel = setup.bumpTravel;
            corner.droopTravel = setup.droopTravel;
            corner.hubCenter = corner.designCenter;
            if (axle.type == VehicleSuspensionType::SolidAxle)
            {
                corner.designContactHeight = -std::max(GetVehicleWheelMount(settings, index).radius, 0.05f);
            }
            else
            {
                suspension::Kinematics kinematics(suspension::Compile(setup.definition));
                suspension::KinematicOutputs design;
                suspension::ComputeOutputs(kinematics, design);
                corner.designContactHeight = design.contactPoint.z;
            }
            vehicle.corners.push_back(std::move(corner));
        }
        for (size_t axle = 0; axle < 2; ++axle)
        {
            const VehicleSuspensionAxle& data = axle == 0 ? settings.frontSuspension : settings.rearSuspension;
            const VehicleCornerSetup& left = setups[2 * axle];
            const VehicleCornerSetup& right = setups[2 * axle + 1];
            if (data.type == VehicleSuspensionType::SolidAxle)
            {
                const double radius = std::max(GetVehicleWheelMount(settings, 2 * axle).radius, 0.05f);
                vehicle.axles[axle] = std::make_unique<suspension::AxleSuspension>(
                    std::make_unique<suspension::SolidAxle>(BuildSolidAxle(data, radius), MakeVehicleCornerUnit(left), MakeVehicleCornerUnit(right)));
            }
            else
            {
                vehicle.axles[axle] = std::make_unique<suspension::AxleSuspension>(
                    std::make_unique<suspension::SuspensionCorner>(left.definition, MakeVehicleCornerUnit(left), suspension::SuspensionCorner::kWheelTravel),
                    std::make_unique<suspension::SuspensionCorner>(right.definition, MakeVehicleCornerUnit(right), suspension::SuspensionCorner::kWheelTravel));
            }
        }
        vehicle.rackAtLock = FitSteeringRackTravel(settings);
    }

    // Before each step: the multibody suspension reads where the physics engine left each wheel, moves
    // the wheel along its linkage (the mount's horizontal place, camber, toe and steering), and hands
    // the physics engine's straight spring the stiffness, damping and preload that make its force, near
    // the current state, the suspension's: springs, damper, stops and anti-roll bar at the wheel, plus
    // the tyre's horizontal forces through the linkage's geometry (anti-dive, anti-squat, jacking).
    //
    // A wheel with an unsprung mass (StepUnsprungCorner) instead moves on its own travel state, and the
    // physics engine's spring carries the tyre's vertical force between the ground and the hub.
    void UpdateCorners(Vehicle& vehicle, float steering)
    {
        if (vehicle.corners.empty())
        {
            return;
        }
        if (!vehicle.body->IsActive())
        {
            // Asleep, the physics engine does not step the car: nothing moves.
            vehicle.lastVelocityValid = false;
            return;
        }
        constexpr double dt = kFixedStepSeconds;
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        const JPH::Quat toBody = vehicle.body->GetRotation().Conjugated();
        const double rack = vehicle.rackAtLock * std::clamp(static_cast<double>(steering), -1.0, 1.0);

        std::array<double, kVehicleWheelCount> travel{};
        for (size_t index = 0; index < vehicle.corners.size(); ++index)
        {
            const Vehicle::Corner& c = vehicle.corners[index];
            travel[index] = c.unsprung ? c.travel : c.designLength - wheels[static_cast<JPH::uint>(index)]->GetSuspensionLength();
        }
        std::array<suspension::CornerInput, kVehicleWheelCount> inputs{};
        std::array<float, kVehicleWheelCount> cosines{};
        for (size_t index = 0; index < vehicle.corners.size(); ++index)
        {
            const Vehicle::Corner& c = vehicle.corners[index];
            const JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
            suspension::CornerInput& in = inputs[index];
            in.dt = dt;
            in.travel = travel[index];
            in.travelRate = c.unsprung ? c.travelRate : (travel[index] - c.travel) / dt;
            in.rack = c.front ? rack : 0.0;
            float cosine = 1.0f;
            if (wheel.HasContact())
            {
                // The road's horizontal push on the tyre over the last step, in the corner's frame.
                const JPH::Vec3 longitudinal = toBody * wheel.GetContactLongitudinal();
                const JPH::Vec3 lateral = toBody * wheel.GetContactLateral();
                const JPH::Vec3 normal = toBody * wheel.GetContactNormal();
                JPH::Vec3 force = longitudinal * (wheel.GetLongitudinalLambda() / static_cast<float>(dt)) + lateral * (wheel.GetLateralLambda() / static_cast<float>(dt));
                if (!vehicle.brushTyres.empty())
                {
                    force = toBody * vehicle.brushWheels[index].force;
                }
                in.load.force = VehicleToCorner(FromJolt(force));
                in.contactNormal = VehicleToCorner(FromJolt(normal));
                cosine = std::max(0.1f, normal.GetY());
            }
            cosines[index] = cosine;
        }
        ApplyAxleTorqueReaction(vehicle);
        for (size_t axle = 0; axle < 2; ++axle)
        {
            vehicle.axles[axle]->Step({inputs[2 * axle], inputs[2 * axle + 1]});
        }
        for (size_t index = 0; index < vehicle.corners.size(); ++index)
        {
            Vehicle::Corner& c = vehicle.corners[index];
            const JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
            JPH::WheelSettingsWV& settings = *vehicle.wheelSettings[index];
            const suspension::CornerInput& in = inputs[index];
            const float cosine = cosines[index];
            const suspension::CornerOutput& out = vehicle.axles[index / 2]->Output(static_cast<int>(index % 2));

            // The anti-roll bar from the two wheels' travel difference.
            const size_t other = index ^ 1u;
            const double arb = -c.antiRollBarRate * (travel[index] - travel[other]);

            if (c.unsprung)
            {
                StepUnsprungCorner(vehicle, c, wheel, settings, out, in, arb, cosine);
                if (std::abs(c.travelRate) > kHubSettledRate)
                {
                    // The physics engine judges sleep by the body alone: not while a hub still moves.
                    vehicle.body->ResetSleepTimer();
                }
            }
            else
            {
                c.travel = travel[index];
                // The road's normal force that balances the travel: F_n * dPn/dz + G = 0.
                const double perTravel = std::max(out.normalPerTravel, 0.2);
                const double generalised = out.strutTravelForce + arb + out.loadTravelForce;
                const double normalForce = -generalised / perTravel;
                const double stiffness = std::max(-(out.strutTravelStiffness - c.antiRollBarRate) / perTravel, 1000.0);
                const double damping = std::max(-out.strutTravelDamping / perTravel, 0.0);
                // The physics engine's spring is k (Lmax + preload - L) - c dL/dt along the normal (its k
                // and c divided by the cosine between the suspension and the normal): match force and slopes.
                settings.mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
                settings.mSuspensionSpring.mStiffness = static_cast<float>(stiffness) * cosine;
                settings.mSuspensionSpring.mDamping = static_cast<float>(damping) * cosine;
                settings.mSuspensionPreloadLength = static_cast<float>((normalForce - stiffness * in.travel - damping * in.travelRate) / stiffness) - settings.mSuspensionMaxLength + c.designLength;
            }

            // The wheel where the linkage has it: the mount above its centre, turned by camber and toe.
            const glm::vec3 center = c.designCenter + CornerToVehicle(out.geometry.wheelCenter);
            settings.mPosition = JPH::Vec3(center.x, c.designCenter.y + c.designLength, center.z);
            const suspension::Vec3 axis = out.geometry.spinAxis;
            const suspension::Vec3 leftward = axis * (index % 2 == 0 ? 1.0 : -1.0);
            const suspension::Vec3 forward = glm::normalize(glm::cross(leftward, suspension::Vec3(0.0, 0.0, 1.0)));
            const suspension::Vec3 up = glm::cross(forward, leftward);
            settings.mWheelForward = ToJolt(CornerToVehicle(forward));
            settings.mWheelUp = ToJolt(CornerToVehicle(up));
            if (c.unsprung)
            {
                // Where the physics engine will look for the ground from in the coming step.
                c.lastMount = vehicle.body->GetWorldTransform() * settings.mPosition;
                c.lastMountValid = true;
                // The hub drawn where this step left it.
                c.hubCenter = center + CornerToVehicle(out.wheelCenterPerTravel) * static_cast<float>(c.travel - in.travel);
            }

            c.camberDegrees = static_cast<float>(out.geometry.camber * 180.0 / std::numbers::pi);
            c.toeDegrees = static_cast<float>(out.geometry.toe * 180.0 / std::numbers::pi);
            c.springForce = static_cast<float>(out.strutForce);
            c.antiRollBarForce = static_cast<float>(-arb);
        }
        vehicle.lastLinearVelocity = vehicle.body->GetLinearVelocity();
        vehicle.lastAngularVelocity = vehicle.body->GetAngularVelocity();
        vehicle.lastVelocityValid = true;
    }

    // A driven solid axle's propshaft torque reaction (Assetto Corsa's TORQUE_REACTION, a signed share
    // of the propshaft's torque taken as a moment about the car's length): on the axle housing, and the
    // opposite on the body. The propshaft's torque is the engine's at the throttle, through the clutch's
    // grip and the gear, times the axle's differential's share of it.
    void ApplyAxleTorqueReaction(Vehicle& vehicle)
    {
        for (size_t axle = 0; axle < 2; ++axle)
        {
            suspension::SolidAxle* solid = vehicle.axles[axle]->Solid();
            const VehicleSuspensionAxle& data = axle == 0 ? vehicle.settings.frontSuspension : vehicle.settings.rearSuspension;
            if (solid == nullptr || data.axleTorqueReaction == 0.0f)
            {
                continue;
            }
            const auto* controller = static_cast<const JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
            double share = 0.0;
            for (const JPH::VehicleDifferentialSettings& differential : controller->GetDifferentials())
            {
                if (differential.mLeftWheel == static_cast<int>(2 * axle) || differential.mRightWheel == static_cast<int>(2 * axle))
                {
                    share += differential.mEngineTorqueRatio;
                }
            }
            const JPH::VehicleTransmission& transmission = controller->GetTransmission();
            const double propshaft = controller->GetEngine().GetTorque(std::max(controller->GetForwardInput(), 0.0f)) * transmission.GetClutchFriction() *
                                     transmission.GetCurrentRatio() * share;
            const double moment = data.axleTorqueReaction * propshaft;
            solid->SetHousingMoment(suspension::Vec3(moment, 0.0, 0.0));
            // The body takes the other end of it, about its own length.
            const JPH::Vec3 onBody = vehicle.body->GetRotation() * JPH::Vec3(0.0f, 0.0f, static_cast<float>(-moment));
            physicsSystem.GetBodyInterfaceNoLock().AddTorque(vehicle.body->GetID(), onBody, JPH::EActivation::DontActivate);
            vehicle.axleTorqueReaction[axle] = static_cast<float>(moment);
        }
    }

    // One step of a wheel with its own mass. The travel z (bump positive) moves by
    //
    //   m_z z'' = F_t (n.dP/dz) + F_h.dP/dz + G(z, z') + G_arb - m_hub f.dW/dz
    //
    // with m_z = m_hub |dW/dz|^2 (W the wheel centre, P the contact point), F_t the tyre's vertical
    // force (rate and damping on its deflection, pushing only), G the springs, damper and stops, and f
    // the specific force (acceleration less gravity) of the body where the hub rides. It is stepped
    // linearly implicit (backward Euler on the slopes), with hard stops at full bump and droop.
    //
    // The physics engine's body is the whole car (hubs included). It receives the tyre's force through
    // its spring, set by the preload to exactly the force the hub took, and the hub's motion relative
    // to the body as -m_hub z'' dW/dz at the hub: together they leave the sprung mass with the
    // suspension's force, and the tyre's friction with the tyre's load.
    void StepUnsprungCorner(Vehicle& vehicle, Vehicle::Corner& c, const JPH::Wheel& wheel, JPH::WheelSettingsWV& settings,
                            const suspension::CornerOutput& out, const suspension::CornerInput& in, double arb, float cosine)
    {
        constexpr double dt = kFixedStepSeconds;
        const JPH::Body& body = *vehicle.body;
        const JPH::Quat rotation = body.GetRotation();
        const JPH::RMat44 transform = body.GetWorldTransform();
        const double z = in.travel;
        const double v = in.travelRate;
        const double perTravel = std::max(out.normalPerTravel, 0.2);

        // The tyre: its deflection from where the ground is now along the suspension. The physics
        // engine found the ground at the start of the last step; the mount has moved since.
        double tyre = 0.0;
        double tyreSlope = 0.0;
        double tyreRateSlope = 0.0;
        double length = settings.mSuspensionMaxLength;
        c.tyreDeflection = 0.0;
        if (wheel.HasContact())
        {
            const JPH::Vec3 normal = wheel.GetContactNormal();
            const JPH::Vec3 direction = rotation * settings.mSuspensionDirection;
            const float along = std::min(normal.Dot(direction), -0.1f);
            const JPH::RVec3 mount = transform * settings.mPosition;
            length = wheel.GetSuspensionLength();
            if (c.lastMountValid)
            {
                length -= normal.Dot(JPH::Vec3(mount - c.lastMount)) / along;
            }
            const double groundRate = -normal.Dot(body.GetPointVelocity(mount) - wheel.GetContactPointVelocity()) / along;
            // Where the unloaded tyre would touch: the design length, less how far its lowest point
            // has risen with the travel.
            const double rise = out.geometry.contactPoint.z - c.designContactHeight;
            const double deflection = (c.designLength - rise) - length;
            if (deflection > 0.0)
            {
                c.tyreDeflection = deflection;
                tyre = c.tyreRate * deflection - c.tyreDamping * (perTravel * v + groundRate);
                if (tyre > 0.0)
                {
                    tyreSlope = -c.tyreRate * perTravel;
                    tyreRateSlope = -c.tyreDamping * perTravel;
                }
                else
                {
                    tyre = 0.0;
                }
            }
        }

        // The body's specific force where the hub rides, in the vehicle's frame.
        const JPH::Vec3 hub = JPH::Vec3(transform * ToJolt(c.hubCenter) - body.GetCenterOfMassPosition());
        const JPH::Vec3 angular = body.GetAngularVelocity();
        JPH::Vec3 acceleration = angular.Cross(angular.Cross(hub));
        if (vehicle.lastVelocityValid)
        {
            acceleration += (body.GetLinearVelocity() - vehicle.lastLinearVelocity) / static_cast<float>(dt) +
                            ((angular - vehicle.lastAngularVelocity) / static_cast<float>(dt)).Cross(hub);
        }
        const JPH::Vec3 specific = rotation.Conjugated() * (acceleration - physicsSystem.GetGravity());
        const suspension::Vec3 hubPerTravel = out.wheelCenterPerTravel;
        const glm::vec3 hubPerTravelVehicle = CornerToVehicle(hubPerTravel);
        const double inertia = -c.hubMass * glm::dot(FromJolt(specific), hubPerTravelVehicle);
        const double mass = std::max(c.hubMass * glm::dot(hubPerTravel, hubPerTravel), 0.5 * c.hubMass);

        const double force = tyre * perTravel + out.loadTravelForce + out.strutTravelForce + arb + inertia;
        const double stiffness = out.strutTravelStiffness - c.antiRollBarRate + tyreSlope * perTravel;
        const double damping = out.strutTravelDamping + tyreRateSlope * perTravel;
        double rate = v + dt * (force + dt * stiffness * v) / (mass - dt * damping - dt * dt * stiffness);
        double next = z + dt * rate;
        if (next > c.bumpTravel)
        {
            next = c.bumpTravel;
            rate = std::min(rate, 0.0);
        }
        else if (next < -c.droopTravel)
        {
            next = -c.droopTravel;
            rate = std::max(rate, 0.0);
        }
        const double travelAccel = (rate - v) / dt;
        c.travel = next;
        c.travelRate = rate;

        // The tyre's force the hub took, for the physics engine to apply to the body (and to bound the
        // tyre's friction): k (Lmax + preload - L) with L where the ground is now.
        const double applied = std::max(tyre + tyreSlope * (next - z) + tyreRateSlope * (rate - v), 0.0);
        settings.mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
        settings.mSuspensionSpring.mStiffness = kTyreCarrierStiffness * cosine;
        settings.mSuspensionSpring.mDamping = 0.0f;
        settings.mSuspensionPreloadLength = std::max(static_cast<float>(applied / kTyreCarrierStiffness + length) - settings.mSuspensionMaxLength, 0.0f);

        // The hub's motion relative to the body, as a force on the body at the hub.
        const JPH::Vec3 relative = rotation * ToJolt(hubPerTravelVehicle) * static_cast<float>(-c.hubMass * travelAccel);
        physicsSystem.GetBodyInterfaceNoLock().AddForce(body.GetID(), relative, transform * ToJolt(c.hubCenter), JPH::EActivation::DontActivate);
    }

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
        float worstSlip = 0.0f;
        bool brush = false;
        const float carSpeed = std::abs((vehicle.body->GetRotation().Conjugated() * vehicle.body->GetLinearVelocity()).GetZ());
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
                float hold = wheel.longitudinalPeakFriction * std::max(wheel.suspensionForce, 0.0f);
                if (wheel.brushTyre)
                {
                    // The brush tyre shares one friction circle between the two directions: what
                    // cornering takes is not there for the drive.
                    hold = std::sqrt(std::max(hold * hold - wheel.lateralForce * wheel.lateralForce, 0.0f));
                    brush = true;
                    worstSlip = std::max(worstSlip, (std::abs(wheel.angularVelocity) * wheel.radius - carSpeed) / std::max(carSpeed, 3.0f));
                }
                holdTorque += hold * wheel.radius;
            }
        }
        const float overall = ratio * controller.GetDifferentials()[0].mDifferentialRatio;
        float engineSide = holdTorque * vehicle.tractionControlGrip / overall;
        if (brush)
        {
            // And past the slip where the brush tyre's drive peaks the clutch lets go further: traction
            // control watching the wheels spin up, as the game's does (SLIP_RATIO_LIMIT).
            constexpr float kSlipLimit = 0.1f;
            engineSide *= std::clamp(2.0f - worstSlip / kSlipLimit, 0.2f, 1.0f);
        }
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
        // The clutch pack's lock follows the torque the engine sends through it.
        const float driveTorque = controller.GetEngine().GetTorque(std::abs(controller.GetForwardInput())) * std::abs(controller.GetTransmission().GetCurrentRatio());
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

    // The brush tyres, called by the vehicle constraint once it has found the ground and before the
    // controller turns the engine and brakes the wheels. Each tyre reads the wheel's motion over the
    // ground in the contact frame, the load the suspension carried in the last step and the camber to
    // the road, steps its carcass, and puts the road's force and aligning moment on the body at the
    // contact and the force's moment about the axle on the wheel's spin. The physics engine's own tyre
    // friction is off (its impulse limits are zero).
    void ApplyBrushTyres(Vehicle& vehicle, JPH::VehicleConstraint& constraint, const JPH::PhysicsStepListenerContext& context)
    {
        const float dt = context.mDeltaTime;
        if (vehicle.brushTyres.empty() || dt <= 0.0f)
        {
            return;
        }
        JPH::Body& body = *constraint.GetVehicleBody();
        JPH::BodyInterface& bodies = context.mPhysicsSystem->GetBodyInterfaceNoLock();
        const JPH::Quat rotation = body.GetRotation();
        const JPH::Array<JPH::Wheel*>& wheels = constraint.GetWheels();
        for (size_t index = 0; index < vehicle.brushTyres.size() && index < wheels.size(); ++index)
        {
            auto& wheel = *static_cast<JPH::WheelWV*>(wheels[static_cast<JPH::uint>(index)]);
            Vehicle::BrushWheel& state = vehicle.brushWheels[index];
            tyre::BrushTyreInput in;
            in.wheelSpeed = wheel.GetAngularVelocity();
            if (!wheel.HasContact())
            {
                state = {};
                state.out = vehicle.brushTyres[index].Step(in, dt);
                continue;
            }
            const JPH::Vec3 normal = wheel.GetContactNormal();
            const JPH::Vec3 longitudinal = wheel.GetContactLongitudinal();
            const JPH::Vec3 left = -wheel.GetContactLateral();
            const JPH::RVec3 position = wheel.GetContactPosition();
            const JPH::Vec3 velocity = body.GetPointVelocity(position) - wheel.GetContactPointVelocity();
            in.forwardVelocity = velocity.Dot(longitudinal);
            in.lateralVelocity = velocity.Dot(left);
            in.yawRate = body.GetAngularVelocity().Dot(normal);
            in.load = std::max(wheel.GetSuspensionLambda() / dt, 0.0f);
            // Camber to the road from the wheel's axle (its right, steered): positive top right.
            JPH::Vec3 forward, up, right;
            constraint.GetWheelLocalBasis(&wheel, forward, up, right);
            in.camber = std::asin(std::clamp(static_cast<double>(-(rotation * right).Dot(normal)), -1.0, 1.0));
            {
                JPH::BodyLockRead lock(context.mPhysicsSystem->GetBodyLockInterfaceNoLock(), wheel.GetContactBodyID());
                in.frictionScale = lock.Succeeded() ? lock.GetBody().GetFriction() : 1.0f;
            }
            const tyre::BrushTyreOutput out = vehicle.brushTyres[index].Step(in, dt);
            const JPH::Vec3 force = longitudinal * static_cast<float>(out.Fx) + left * static_cast<float>(out.Fy);
            bodies.AddForce(body.GetID(), force, position, JPH::EActivation::DontActivate);
            bodies.AddTorque(body.GetID(), normal * static_cast<float>(out.Mz), JPH::EActivation::DontActivate);
            wheel.ApplyTorque(static_cast<float>(-out.Fx * out.effectiveRadius + out.rollingResistanceTorque), dt);
            state.out = out;
            state.force = force;
            state.load = static_cast<float>(in.load);
            state.contact = true;
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
            if (index < vehicle.corners.size())
            {
                const Vehicle::Corner& corner = vehicle.corners[index];
                state.multibody = true;
                state.travel = static_cast<float>(corner.travel);
                state.camberDegrees = corner.camberDegrees;
                state.toeDegrees = corner.toeDegrees;
                state.springForce = corner.springForce;
                state.antiRollBarForce = corner.antiRollBarForce;
                if (corner.unsprung)
                {
                    // The physics engine's wheel sits on the ground; the hub is where its own motion
                    // has it, the tyre squashed between them.
                    state.unsprungMass = true;
                    state.tyreDeflection = static_cast<float>(corner.tyreDeflection);
                    state.pose.position = FromJolt(vehicle.body->GetWorldTransform() * ToJolt(corner.hubCenter));
                    state.suspensionLength = corner.designLength - (corner.hubCenter.y - corner.designCenter.y);
                }
            }
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
            if (index < vehicle.brushTyres.size())
            {
                // The brush tyre's forces and slips replace the physics engine's (contactLateral is the right).
                const Vehicle::BrushWheel& brush = vehicle.brushWheels[index];
                const tyre::BrushTyreState& tyreState = vehicle.brushTyres[index].State();
                state.brushTyre = true;
                state.carcassDeflection = glm::vec3(static_cast<float>(tyreState.carcass[0]), static_cast<float>(tyreState.carcass[1]), static_cast<float>(tyreState.carcass[2]));
                if (state.inContact && brush.contact)
                {
                    const float load = std::max(brush.load, 1.0f);
                    state.longitudinalForce = static_cast<float>(brush.out.Fx);
                    state.lateralForce = static_cast<float>(-brush.out.Fy);
                    state.aligningTorque = static_cast<float>(brush.out.Mz);
                    state.slidingShare = static_cast<float>(brush.out.slidingShare);
                    state.slipRatio = static_cast<float>(brush.out.slipRatio);
                    state.slipAngleDegrees = static_cast<float>(brush.out.slipAngle * 180.0 / std::numbers::pi);
                    state.longitudinalFriction = static_cast<float>(std::abs(brush.out.Fx)) / load;
                    state.lateralFriction = static_cast<float>(std::abs(brush.out.Fy)) / load;
                    state.longitudinalPeakFriction = static_cast<float>(brush.out.peakFriction);
                    state.lateralPeakFriction = static_cast<float>(brush.out.peakFriction);
                }
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
    if (settings.inertiaBox.x > 0.0f && settings.inertiaBox.y > 0.0f && settings.inertiaBox.z > 0.0f)
    {
        // The car's data gives its inertia as a uniform box (width, height, length) of its mass about
        // the centre of mass, not the collision box's.
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        bodySettings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(ToJolt(settings.inertiaBox), 1.0f);
        bodySettings.mMassPropertiesOverride.ScaleToMass(std::max(settings.massKg, 1.0f));
    }
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
    vehicle.settings = settings;
    impl.BuildCorners(vehicle);
    if (settings.tyreModel == VehicleTyreModel::Brush)
    {
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            vehicle.brushTyres.emplace_back(BuildBrushTyreParameters(settings, index));
        }
        // The physics engine's tyre friction off: the brush tyres push instead.
        auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
        controller->SetTireMaxImpulseCallback(
            [](JPH::uint, float& outLongitudinalImpulse, float& outLateralImpulse, float, float, float, float, float, float)
            {
                outLongitudinalImpulse = 0.0f;
                outLateralImpulse = 0.0f;
            });
        const VehicleId id = static_cast<VehicleId>(impl.vehicles.size());
        vehicle.constraint->SetPostCollideCallback(
            [&impl, id](JPH::VehicleConstraint& constraint, const JPH::PhysicsStepListenerContext& context)
            {
                impl.ApplyBrushTyres(impl.vehicles[id], constraint, context);
            });
    }

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
    for (tyre::BrushTyre& tyre : vehicle.brushTyres)
    {
        tyre.Reset();
    }
    vehicle.brushWheels = {};
    m_impl->BuildCorners(vehicle);
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

VehicleLinkage PhysicsWorld::GetVehicleLinkage(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    VehicleLinkage linkage;
    if (vehicle.corners.size() < kVehicleWheelCount)
    {
        return linkage;
    }
    const JPH::RMat44 transform = vehicle.body->GetWorldTransform();
    for (size_t axle = 0; axle < 2; ++axle)
    {
        // The axle's frame from its left wheel's: that wheel's design centre is half a track to the left.
        const Impl::Vehicle::Corner& left = vehicle.corners[2 * axle];
        const Impl::Vehicle::Corner& right = vehicle.corners[2 * axle + 1];
        const double halfTrack = vehicle.axles[axle]->SketchHalfTrack(0.5 * std::abs(left.designCenter.x - right.designCenter.x));
        suspension::LinkageSketch sketch;
        vehicle.axles[axle]->Sketch(halfTrack, sketch);
        const auto toWorld = [&](const suspension::Vec3& p) {
            const glm::vec3 inVehicle = left.designCenter + CornerToVehicle(p - suspension::Vec3(0.0, halfTrack, 0.0));
            return FromJolt(transform * ToJolt(inVehicle));
        };
        for (const auto& [a, b] : sketch.links)
        {
            linkage.links.push_back({toWorld(a), toWorld(b)});
        }
        for (const auto& [a, b] : sketch.carriers)
        {
            linkage.carriers.push_back({toWorld(a), toWorld(b)});
        }
        for (const suspension::Vec3& p : sketch.chassis)
        {
            linkage.chassis.push_back(toWorld(p));
        }
        for (const suspension::Vec3& p : sketch.joints)
        {
            linkage.joints.push_back(toWorld(p));
        }
    }
    return linkage;
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
            impl.UpdateCorners(vehicle, input.right);
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
