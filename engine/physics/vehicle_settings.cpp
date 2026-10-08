#include "vehicle_settings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

namespace me
{

namespace
{
// Below this the car counts as stopped, and throttle the other way selects the other direction.
constexpr float kDirectionChangeSpeed = 0.5f; // m/s
constexpr float kGravity = 9.81f;

// A curve's value at x by linear interpolation, its end values beyond the ends; 0 for no points.
float EvaluateCurve(const std::vector<glm::vec2>& curve, float x)
{
    if (curve.empty())
    {
        return 0.0f;
    }
    if (x <= curve.front().x)
    {
        return curve.front().y;
    }
    for (size_t index = 1; index < curve.size(); ++index)
    {
        if (x <= curve[index].x)
        {
            const float span = curve[index].x - curve[index - 1].x;
            const float t = span > 0.0f ? (x - curve[index - 1].x) / span : 1.0f;
            return curve[index - 1].y + (curve[index].y - curve[index - 1].y) * t;
        }
    }
    return curve.back().y;
}
}

float ComputeBrakeTorquePerWheel(const VehicleSettings& settings)
{
    if (settings.maxBrakeTorque > 0.0f)
    {
        return settings.maxBrakeTorque;
    }
    // Without tyre data the physics engine's own tyres peak at 1.2 and are combined with the surface's
    // friction (1 by default) by a square root.
    constexpr float kDefaultGrip = 1.1f;
    // Sharing the brakes by load gives the fronts more of the torque than the fixed split does, and they
    // lock first: the total is held a little lower so that the car keeps its steering.
    const float gripUsed = settings.dynamicBrakeBias ? 0.66f : 0.75f;
    const float front = 0.5f * (settings.tyres[0].longitudinalGrip + settings.tyres[1].longitudinalGrip);
    const float rear = 0.5f * (settings.tyres[2].longitudinalGrip + settings.tyres[3].longitudinalGrip);
    const float grip = front > 0.0f && rear > 0.0f ? 0.5f * (front + rear) : std::max(front, rear) > 0.0f ? std::max(front, rear) : kDefaultGrip;
    return gripUsed * grip * std::max(settings.massKg, 1.0f) * kGravity * std::max(settings.wheelRadius, 0.01f) * 0.25f;
}

VehicleSettings FitVehicleSettingsToBounds(
    const glm::vec3& minBounds,
    const glm::vec3& maxBounds,
    const VehicleSettings& tuning,
    const VehicleWheelLayout* wheelLayout)
{
    // A model with no extent along an axis (a placeholder, a flat card) still gets a drivable car.
    constexpr glm::vec3 kMinimumSize(0.5f, 0.3f, 0.8f);
    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 size = glm::max(maxBounds - minBounds, kMinimumSize);
    const float floorY = center.y - size.y * 0.5f;

    VehicleSettings settings = tuning;
    // A road car 1.4 m tall rides on wheels of about 0.32 m.
    settings.wheelRadius = std::clamp(size.y * 0.23f, 0.12f, 0.6f);
    settings.wheelWidth = std::clamp(size.x * 0.12f, 0.08f, 0.4f);
    settings.trackCenterX = center.x;
    settings.halfTrackWidth = std::max(size.x * 0.5f - settings.wheelWidth * 0.6f, settings.wheelWidth);
    // Overhangs of about a fifth of the length at either end.
    settings.frontAxleZ = center.z + size.z * 0.5f - std::max(size.z * 0.18f, settings.wheelRadius * 1.1f);
    settings.rearAxleZ = center.z - size.z * 0.5f + std::max(size.z * 0.2f, settings.wheelRadius * 1.1f);
    settings.hasWheelLayout = wheelLayout != nullptr;
    glm::vec3 centerOfWheels(0.0f);
    if (wheelLayout != nullptr)
    {
        // Left wheels are the even ones: +X is the car's left.
        settings.wheelLayout = *wheelLayout;
        float radius = 0.0f;
        float width = 0.0f;
        glm::vec3 sum(0.0f);
        for (const VehicleWheelGeometry& wheel : *wheelLayout)
        {
            radius += wheel.radius * 0.25f;
            width += wheel.width * 0.25f;
            sum += wheel.center * 0.25f;
        }
        settings.wheelRadius = std::clamp(radius, 0.05f, 1.0f);
        settings.wheelWidth = std::clamp(width, 0.05f, 0.6f);
        centerOfWheels = sum;
        settings.trackCenterX = sum.x;
        settings.halfTrackWidth = ((*wheelLayout)[0].center.x - (*wheelLayout)[1].center.x) * 0.5f;
        settings.frontAxleZ = ((*wheelLayout)[0].center.z + (*wheelLayout)[1].center.z) * 0.5f;
        settings.rearAxleZ = ((*wheelLayout)[2].center.z + (*wheelLayout)[3].center.z) * 0.5f;
    }
    settings.suspensionMaxLength = std::clamp(settings.wheelRadius, 0.15f, 0.35f);
    settings.suspensionMinLength = settings.suspensionMaxLength * 0.15f;

    // The contact patch sits on the floor once the springs carry the car's weight.
    const float restLength = ComputeRestSuspensionLength(settings, kGravity);
    settings.wheelMountY = (wheelLayout != nullptr ? centerOfWheels.y : floorY + settings.wheelRadius) + restLength;

    // The chassis box clears the ground by most of a wheel radius and reaches the roof.
    const float chassisBottom = floorY + settings.wheelRadius * 0.8f;
    const float chassisTop = std::max(center.y + size.y * 0.5f, chassisBottom + 0.1f);
    settings.chassisCenter = glm::vec3(center.x, (chassisBottom + chassisTop) * 0.5f, center.z);
    settings.chassisHalfExtents = glm::vec3(size.x * 0.5f, (chassisTop - chassisBottom) * 0.5f, size.z * 0.5f);

    // A road car's centre of mass is around a third of its height above the ground, over the middle of
    // the model; the car's own data says where (its weight split between the axles, its height over
    // the ground under the tyres).
    const float groundY = wheelLayout != nullptr ? centerOfWheels.y - settings.wheelRadius : floorY;
    const float centerOfMassY = settings.centerOfMassHeight > 0.0f ? groundY + settings.centerOfMassHeight
                                                                  : floorY + std::max(size.y * 0.33f, settings.wheelRadius * 1.2f);
    glm::vec3 centerOfMass(settings.chassisCenter.x, centerOfMassY, settings.chassisCenter.z);
    if (settings.frontWeightShare > 0.0f)
    {
        centerOfMass.x = settings.trackCenterX;
        centerOfMass.z = settings.rearAxleZ + settings.frontWeightShare * (settings.frontAxleZ - settings.rearAxleZ);
    }
    settings.centerOfMassOffset = centerOfMass - settings.chassisCenter;
    return settings;
}

bool HasSuspensionGeometry(const VehicleSettings& settings)
{
    return settings.frontSuspension.type != VehicleSuspensionType::None && settings.rearSuspension.type != VehicleSuspensionType::None;
}

VehicleCarSpec WithStartingFuel(const VehicleCarSpec& spec)
{
    VehicleCarSpec fuelled = spec;
    fuelled.fuelLitres.reset();
    fuelled.fuelTankPosition.reset();
    if (!spec.fuelLitres.has_value() || *spec.fuelLitres <= 0.0f || !spec.massKg.has_value() || *spec.massKg <= 0.0f)
    {
        return fuelled;
    }
    const float fuel = *spec.fuelLitres * kFuelKgPerLitre;
    const glm::vec3 tank = spec.fuelTankPosition.value_or(glm::vec3(0.0f));
    fuelled.massKg = *spec.massKg + fuel;
    // The fuel joins the body, whose centre of mass the weight split and the axles' heights place: the
    // car's mass less the hubs that ride on their own tyre springs (as PhysicsWorld takes it).
    float hubs = 0.0f;
    const bool linkage = spec.frontSuspension.has_value() && spec.rearSuspension.has_value() &&
                         spec.frontSuspension->type != VehicleSuspensionType::None && spec.rearSuspension->type != VehicleSuspensionType::None;
    for (const std::optional<VehicleSuspensionAxle>* axle : {&spec.frontSuspension, &spec.rearSuspension})
    {
        if (linkage && (*axle)->hubMass > 0.0f && (*axle)->tyreRate > 0.0f)
        {
            hubs += 2.0f * (*axle)->hubMass;
        }
    }
    const float body = std::max(*spec.massKg - hubs, 0.5f * *spec.massKg);
    // The tank's share of its weight on the front axle: its distance ahead of the rear axle over the
    // wheelbase (the body's centre of mass being frontWeightShare of the wheelbase ahead of it).
    if (spec.frontWeightShare.has_value() && spec.wheelbase.has_value() && *spec.wheelbase > 0.0f)
    {
        const float tankShare = *spec.frontWeightShare + tank.z / *spec.wheelbase;
        fuelled.frontWeightShare = (*spec.frontWeightShare * body + tankShare * fuel) / (body + fuel);
    }
    // The body's centre of mass moves up or down by the fuel's moment over its new mass.
    const float rise = fuel * tank.y / (body + fuel);
    for (std::optional<VehicleSuspensionAxle>* axle : {&fuelled.frontSuspension, &fuelled.rearSuspension})
    {
        if (axle->has_value())
        {
            (*axle)->centerOfMassAboveWheel += rise;
        }
    }
    return fuelled;
}

float ReadVehicleControllerInput(const VehicleControllerInputs& inputs, const std::string& name)
{
    static constexpr std::pair<std::string_view, float VehicleControllerInputs::*> kInputs[] = {
        {"STEER_DEG", &VehicleControllerInputs::steerDegrees},
        {"SPEED_KMH", &VehicleControllerInputs::speedKmh},
        {"GAS", &VehicleControllerInputs::gas},
        {"BRAKE", &VehicleControllerInputs::brake},
        {"LATG", &VehicleControllerInputs::lateralG},
        {"GEAR", &VehicleControllerInputs::gear},
        {"SLIPANGLE_FRONT_AVERAGE", &VehicleControllerInputs::slipAngleFrontAverage},
        {"SLIPANGLE_FRONT_MAX", &VehicleControllerInputs::slipAngleFrontMax},
        {"SLIPANGLE_REAR_AVERAGE", &VehicleControllerInputs::slipAngleRearAverage},
        {"SLIPANGLE_REAR_MAX", &VehicleControllerInputs::slipAngleRearMax},
        {"OVERSTEER_FACTOR", &VehicleControllerInputs::oversteerFactor}};
    for (const auto& [key, member] : kInputs)
    {
        if (name == key)
        {
            return inputs.*member;
        }
    }
    return 0.0f;
}

float EvaluateVehicleControllers(const std::vector<VehicleController>& controllers, const VehicleControllerInputs& inputs, std::vector<float>& filtered, float dt)
{
    // The game steps its physics at 333 Hz; a FILTER is its factor per step there.
    constexpr float kGameStepsPerSecond = 333.0f;
    const bool fresh = filtered.size() != controllers.size();
    if (fresh)
    {
        filtered.assign(controllers.size(), 0.0f);
    }
    float value = 0.0f;
    for (size_t index = 0; index < controllers.size(); ++index)
    {
        const VehicleController& controller = controllers[index];
        float x = EvaluateCurve(controller.curve, ReadVehicleControllerInput(inputs, controller.input));
        if (!fresh && dt > 0.0f && controller.filter > 0.0f && controller.filter < 1.0f)
        {
            const float a = std::pow(controller.filter, dt * kGameStepsPerSecond);
            x = a * filtered[index] + (1.0f - a) * x;
        }
        filtered[index] = x;
        value = controller.combinator == "MULT" ? value * x : value + x;
        if (controller.upLimit > controller.downLimit)
        {
            value = std::clamp(value, controller.downLimit, controller.upLimit);
        }
    }
    return value;
}

float ComputeCentreCouplingTorque(float rampTorque, float maxTorque, float finalDrive, float rearWheelSpeed, float frontWheelSpeed)
{
    const float shaftSlip = std::max(finalDrive, 0.0f) * (rearWheelSpeed - frontWheelSpeed);
    const float limit = std::max(maxTorque, 0.0f);
    return std::clamp(std::max(rampTorque, 0.0f) * shaftSlip, -limit, limit);
}

VehicleChassisParts BuildChassisParts(const VehicleSettings& settings)
{
    VehicleChassisParts parts;
    if (settings.carColliders.empty())
    {
        return parts;
    }
    const glm::vec3 centerOfMass = settings.chassisCenter + settings.centerOfMassOffset;
    float groundTop = -std::numeric_limits<float>::max();
    for (const VehicleColliderBox& box : settings.carColliders)
    {
        const glm::vec3 half = glm::max(glm::abs(box.size) * 0.5f, glm::vec3(0.01f));
        parts.boxes.push_back(VehicleChassisBox{centerOfMass + box.center, half});
        if (box.groundEnabled)
        {
            groundTop = std::max(groundTop, centerOfMass.y + box.center.y + half.y);
        }
    }
    parts.hull.reserve(settings.chassisHull.size());
    for (glm::vec3 point : settings.chassisHull)
    {
        point.y = std::max(point.y, groundTop);
        parts.hull.push_back(point);
    }
    return parts;
}

float VehicleTurboBoost(const VehicleTurbo& turbo, float rpm, float throttle)
{
    // As gro-ove's ac-torque-helper (src/acTurbo.jsx, calculateMultipler) has the game: the maximum
    // boost scaled by the revs, then cut at the wastegate. A turbo whose maximum is above its wastegate
    // so reaches the wastegate's level below the reference rpm. The gas pedal multiplies the turbo's
    // activation before gamma is applied (as Custom Shaders Patch documents the game's, EXT_GAS_CURVE):
    // with gamma 2, half throttle asks a quarter of the boost.
    const float gas = std::clamp(throttle, 0.0f, 1.0f);
    float level = std::max(turbo.maxBoost, 0.0f);
    if (turbo.referenceRpm > 0.0f)
    {
        level *= std::clamp(std::pow(gas * std::max(rpm, 0.0f) / turbo.referenceRpm, std::max(turbo.gamma, 0.0f)), 0.0f, 1.0f);
    }
    else
    {
        level *= std::pow(gas, std::max(turbo.gamma, 0.0f));
    }
    if (turbo.wastegate > 0.0f)
    {
        level = std::min(level, turbo.wastegate);
    }
    return level;
}

float VehicleTurboTorqueScale(const VehicleSettings& settings, float rpm, float boost)
{
    if (settings.turbos.empty())
    {
        return 1.0f;
    }
    float full = 0.0f;
    for (const VehicleTurbo& turbo : settings.turbos)
    {
        full += VehicleTurboBoost(turbo, rpm, 1.0f);
    }
    const float total = EvaluateCurve(settings.torqueCurve, rpm);
    if (total <= 0.0f)
    {
        return 1.0f;
    }
    // A hybrid's motor torque in the curve is not the turbos' to scale.
    const float motor = settings.ersDelivery == VehicleErsDelivery::AddedToEngine ? EvaluateCurve(settings.ersTorqueCurve, rpm) : 0.0f;
    const float engine = std::max(total - motor, 0.0f);
    return (motor + engine * (1.0f + boost) / (1.0f + full)) / total;
}

std::vector<glm::vec2> AddTorqueCurves(const std::vector<glm::vec2>& a, const std::vector<glm::vec2>& b)
{
    std::vector<float> rpms;
    rpms.reserve(a.size() + b.size());
    for (const glm::vec2& point : a)
    {
        rpms.push_back(point.x);
    }
    for (const glm::vec2& point : b)
    {
        rpms.push_back(point.x);
    }
    std::sort(rpms.begin(), rpms.end());
    rpms.erase(std::unique(rpms.begin(), rpms.end()), rpms.end());
    std::vector<glm::vec2> sum;
    sum.reserve(rpms.size());
    for (const float rpm : rpms)
    {
        sum.emplace_back(rpm, EvaluateCurve(a, rpm) + EvaluateCurve(b, rpm));
    }
    return sum;
}

float StaticTyreLoad(const VehicleCarSpec& dryspec, bool front)
{
    if (!dryspec.massKg.has_value() || *dryspec.massKg <= 0.0f)
    {
        return 0.0f;
    }
    // As the kn5 import took it: the share of CG_LOCATION, or half.
    const float frontWeight = dryspec.frontWeightShare.value_or(0.5f);
    return *dryspec.massKg * kGravity * (front ? frontWeight : 1.0f - frontWeight) * 0.5f;
}

VehicleTyreSettings TyreSettingsFromSpec(const tyre::TyreSpec& spec, float staticLoadNewtons)
{
    VehicleTyreSettings settings;
    settings.longitudinalGrip = tyre::LongitudinalGripAtLoad(spec, staticLoadNewtons);
    settings.lateralGrip = tyre::LateralGripAtLoad(spec, staticLoadNewtons);
    if (const float limitAngle = spec.slip.frictionLimitAngleDegrees.value_or(0.0f); limitAngle > 0.0f)
    {
        settings.peakSlipAngleDegrees = limitAngle;
        // A brush tyre reaches its longitudinal peak at about the slip the lateral one does.
        settings.peakSlipRatio = std::tan(limitAngle * std::numbers::pi_v<float> / 180.0f);
    }
    if (const float falloff = spec.slip.falloffLevel.value_or(0.0f); falloff > 0.0f && falloff <= 1.0f)
    {
        settings.postPeakShare = falloff;
    }
    settings.inertia = spec.size.angularInertia.value_or(0.0f);
    // How the grip falls with load: LS_EXPX along the wheel, LS_EXPY across it (the peak force grows as the
    // load to that power); one alone stands for both.
    const float x = spec.grip.longitudinalLoadExponent.value_or(0.0f);
    const float y = spec.grip.lateralLoadExponent.value_or(0.0f);
    if (x > 0.0f || y > 0.0f)
    {
        settings.longitudinalLoadExponent = std::min(x > 0.0f ? x : y, 1.0f);
        settings.lateralLoadExponent = std::min(y > 0.0f ? y : x, 1.0f);
    }
    // The brush tyre's build: RIM_RADIUS, PRESSURE_STATIC (psi), RELAXATION_LENGTH and CX_MULT.
    const auto positive = [](const std::optional<float>& value) { return std::max(value.value_or(0.0f), 0.0f); };
    constexpr float kPascalsPerPsi = 6894.757f;
    settings.rimRadius = positive(spec.size.rimRadius);
    settings.inflationPressure = positive(spec.pressure.staticPsi) * kPascalsPerPsi;
    settings.relaxationLength = positive(spec.slip.relaxationLength);
    settings.longitudinalStiffnessRatio = positive(spec.slip.longitudinalStiffnessRatio);
    settings.verticalRate = positive(spec.vertical.rate);
    settings.verticalDamping = positive(spec.vertical.damping);
    settings.rollingResistance0 = positive(spec.rolling.resistance0);
    settings.rollingResistance1 = positive(spec.rolling.resistance1);
    settings.rollingResistanceSlip = positive(spec.rolling.resistanceSlip);
    // The game reads RADIUS_ANGULAR_K in millimetres.
    settings.radiusGrowth = positive(spec.size.radiusGrowthMm) * 0.001f;
    settings.camberGain = spec.camber.gain.value_or(0.0f);
    settings.dcamber0 = spec.camber.dcamber0.value_or(0.0f);
    settings.dcamber1 = spec.camber.dcamber1.value_or(0.0f);
    settings.speedSensitivity = positive(spec.grip.speedSensitivity);
    settings.brakeLongitudinalMod = spec.grip.brakeLongitudinalMod.value_or(0.0f);
    settings.referenceLoad = positive(spec.grip.referenceLoad);
    settings.flexGain = spec.carcass.flexGain.value_or(0.0f);
    settings.combinedFactor = positive(spec.slip.combinedFactor);
    return settings;
}

// Assetto Corsa's tyre model V10 as acs.exe has it (Tyre::addTyreForcesV10, SCTM::solve, Tyre::addGroundContact;
// read from the game's own symbols, docs/design/2026-10-08-tyre-data-terms-design.md). Only what sits on top of
// a slip curve: the curve itself stays the brush tyre's.
VehicleTyreStepTerms ComputeTyreStepTerms(const VehicleTyreSettings& tyre, const VehicleTyreMotion& motion)
{
    VehicleTyreStepTerms terms;
    terms.lateralVelocity = motion.lateralVelocity;
    const float vx = motion.forwardVelocity;
    const float vy = motion.lateralVelocity;
    const float absVx = std::abs(vx);
    // The slip angle (positive moving left, which the road pushes back to the right) and camber's thrust in
    // it: alpha + sin(camber) * CAMBER_GAIN. A camber to the right pushes right, as a slip angle to the left does.
    constexpr float kLimitAngle = 1.5f;
    constexpr float kSlowest = 1e-3f;
    const float alpha = absVx > kSlowest ? std::atan(vy / absVx) : (vy > 0.0f ? kLimitAngle : vy < 0.0f ? -kLimitAngle : 0.0f);
    float shifted = alpha;
    if (tyre.camberGain != 0.0f)
    {
        shifted = std::clamp(alpha + std::sin(motion.camber) * tyre.camberGain, -kLimitAngle, kLimitAngle);
        if (absVx > kSlowest && std::abs(alpha) < kLimitAngle)
        {
            terms.lateralVelocity = absVx * std::tan(shifted);
        }
    }
    // Lateral grip by camber: over 1 + DCAMBER_0 g - DCAMBER_1 g^2 (held above -1 + 0.1), g the camber with the
    // sign that makes leaning the way the force pushes negative (the grip's peak, DCAMBER_0 / (2 DCAMBER_1)).
    if (tyre.dcamber0 != 0.0f || tyre.dcamber1 != 0.0f)
    {
        const float camber = motion.camber;
        const bool leaningWithForce = (camber >= 0.0f && shifted >= 0.0f) || (camber < 0.0f && shifted <= 0.0f);
        const float g = leaningWithForce ? -std::abs(camber) : std::abs(camber);
        float sum = tyre.dcamber0 * g - tyre.dcamber1 * g * g;
        if (!(sum > -1.0f))
        {
            sum = -0.9f;
        }
        terms.axisFrictionScale[1] /= 1.0f + sum;
    }
    // Both grips over 1 + SPEED_SENSITIVITY times the contact's sliding speed.
    const float treadSpeed = motion.wheelSpeed * motion.radius;
    const float slideX = treadSpeed - vx;
    if (tyre.speedSensitivity > 0.0f)
    {
        const float scale = 1.0f / (1.0f + tyre.speedSensitivity * std::sqrt(vy * vy + slideX * slideX));
        terms.axisFrictionScale[0] *= scale;
        terms.axisFrictionScale[1] *= scale;
    }
    // Braking (a negative slip ratio, the tread slower than the ground) the longitudinal grip takes BRAKE_DX_MOD.
    if (tyre.brakeLongitudinalMod != 0.0f && slideX < 0.0f)
    {
        terms.axisFrictionScale[0] *= 1.0f + tyre.brakeLongitudinalMod;
    }
    // The tyre grows with its spin.
    const float spin = std::abs(motion.wheelSpeed);
    terms.radiusGrowth = tyre.radiusGrowth * spin;
    // Rolling resistance, a coefficient on load times radius: ROLLING_RESISTANCE_0 (the brush tyre's own) and
    // _1 on the tread's speed squared above 1 rad/s, and above 20 rad/s all of it times 1 + _SLIP / 1000 times
    // the slip over its peak (at most 1).
    if (spin > 1.0f && (tyre.rollingResistance1 > 0.0f || tyre.rollingResistanceSlip > 0.0f))
    {
        const float speed = (motion.radius + terms.radiusGrowth) * motion.wheelSpeed;
        const float base = 0.001f * (tyre.rollingResistance0 + tyre.rollingResistance1 * speed * speed);
        float coefficient = base;
        if (spin > 20.0f && tyre.rollingResistanceSlip > 0.0f && tyre.peakSlipAngleDegrees > 0.0f)
        {
            // The theoretical slip (kappa and tan(alpha) over 1 + kappa), combined by COMBINED_FACTOR's norm,
            // over the peak's, which FLEX_GAIN moves with load (to tan((1 + FLEX_GAIN) angle) at twice FZ0).
            const float kappa = std::max(absVx > kSlowest ? slideX / absVx : 0.0f, -0.99999f);
            const float sx = kappa / (1.0f + kappa);
            const float sy = std::tan(shifted) / (1.0f + kappa);
            const float exponent = tyre.combinedFactor > 0.0f ? tyre.combinedFactor : 2.0f;
            const float slip = exponent == 2.0f ? std::sqrt(sx * sx + sy * sy)
                                                : std::pow(std::pow(std::abs(sx), exponent) + std::pow(std::abs(sy), exponent), 1.0f / exponent);
            const float degrees = std::numbers::pi_v<float> / 180.0f;
            const float atReference = std::tan(tyre.peakSlipAngleDegrees * degrees);
            const float atDouble = std::tan(tyre.peakSlipAngleDegrees * (1.0f + tyre.flexGain) * degrees);
            const float loadShare = tyre.referenceLoad > 0.0f ? (motion.load - tyre.referenceLoad) / tyre.referenceLoad : 0.0f;
            const float peak = std::max(atReference + loadShare * (atDouble - atReference), 1e-4f);
            coefficient *= 1.0f + 0.001f * tyre.rollingResistanceSlip * std::clamp(slip / peak, 0.0f, 1.0f);
        }
        terms.extraRollingResistance = coefficient - 0.001f * tyre.rollingResistance0;
    }
    return terms;
}

void SetAxleTyres(VehicleSettings& settings, bool front, const VehicleTyreSettings& tyres)
{
    settings.tyres[front ? 0 : 2] = tyres;
    settings.tyres[front ? 1 : 3] = tyres;
}

VehicleSettings ApplyCarSpec(const VehicleSettings& tuning, const VehicleCarSpec& dryspec)
{
    const VehicleCarSpec spec = WithStartingFuel(dryspec);
    VehicleSettings settings = tuning;
    if (spec.massKg.has_value() && *spec.massKg > 0.0f)
    {
        settings.massKg = *spec.massKg;
    }
    if (spec.frontWeightShare.has_value() && *spec.frontWeightShare > 0.0f && *spec.frontWeightShare < 1.0f)
    {
        settings.frontWeightShare = *spec.frontWeightShare;
    }
    if (spec.inertiaBox.has_value() && spec.inertiaBox->x > 0.0f && spec.inertiaBox->y > 0.0f && spec.inertiaBox->z > 0.0f)
    {
        settings.inertiaBox = *spec.inertiaBox;
    }
    // The centre of mass's height: each axle's tyre radius plus its height over the wheel centre
    // (centerOfMassAboveWheel, Assetto Corsa's -BASEY), by the weight on it.
    if (spec.frontSuspension.has_value() && spec.rearSuspension.has_value() && spec.frontSuspension->tyreRadius > 0.0f &&
        spec.rearSuspension->tyreRadius > 0.0f)
    {
        const float share = settings.frontWeightShare > 0.0f ? settings.frontWeightShare : 0.5f;
        const float front = spec.frontSuspension->tyreRadius + spec.frontSuspension->centerOfMassAboveWheel;
        const float rear = spec.rearSuspension->tyreRadius + spec.rearSuspension->centerOfMassAboveWheel;
        const float height = share * front + (1.0f - share) * rear;
        if (height > 0.0f)
        {
            settings.centerOfMassHeight = height;
        }
    }
    if (spec.drive.has_value())
    {
        settings.drive = *spec.drive;
    }
    if (spec.minRpm.has_value() && *spec.minRpm > 0.0f)
    {
        settings.minRpm = *spec.minRpm;
    }
    if (spec.maxRpm.has_value() && *spec.maxRpm > settings.minRpm)
    {
        settings.maxRpm = *spec.maxRpm;
    }

    if (spec.torqueCurve.size() >= 2)
    {
        const auto byRpm = [](const glm::vec2& a, const glm::vec2& b)
        {
            return a.x < b.x;
        };
        settings.torqueCurve = spec.torqueCurve;
        std::sort(settings.torqueCurve.begin(), settings.torqueCurve.end(), byRpm);
        // The curve has their full boost (the import's AcCarData::TurboBoost); they spool at run time.
        settings.turbos = spec.turbos;
        settings.ersTorqueCurve.clear();
        settings.ersDelivery = VehicleErsDelivery::None;
        if (spec.ers.has_value() && spec.ers->torqueCurve.size() >= 2)
        {
            settings.ersTorqueCurve = spec.ers->torqueCurve;
            std::sort(settings.ersTorqueCurve.begin(), settings.ersTorqueCurve.end(), byRpm);
            settings.ersDelivery = VehicleErsDelivery::AddedToEngine;
            settings.torqueCurve = AddTorqueCurves(settings.torqueCurve, settings.ersTorqueCurve);
        }
        float peak = 0.0f;
        for (const glm::vec2& point : settings.torqueCurve)
        {
            peak = std::max(peak, point.y);
        }
        if (peak > 0.0f)
        {
            settings.maxEngineTorque = peak;
        }
        else
        {
            settings.torqueCurve.clear();
        }
    }

    if (!spec.gearRatios.empty())
    {
        settings.gearRatios = spec.gearRatios;
        if (spec.reverseGearRatio.has_value())
        {
            settings.reverseGearRatio = -std::abs(*spec.reverseGearRatio);
        }
        if (spec.finalDriveRatio.has_value() && *spec.finalDriveRatio > 0.0f)
        {
            settings.finalDriveRatio = *spec.finalDriveRatio;
        }
        // The shift points follow the engine's rev range (ComputeVehicleShiftPoints), changing up on full
        // throttle where the car's own automatic gearbox does when its data says.
        settings.shiftUpRpm = 0.0f;
        settings.shiftDownRpm = 0.0f;
        if (spec.autoShiftUpRpm.has_value() && *spec.autoShiftUpRpm > settings.minRpm && *spec.autoShiftUpRpm <= settings.maxRpm)
        {
            settings.shiftUpRpm = *spec.autoShiftUpRpm;
        }
    }

    if (spec.gearSwitchSeconds.has_value() && *spec.gearSwitchSeconds > 0.0f)
    {
        settings.gearSwitchSeconds = *spec.gearSwitchSeconds;
    }
    if (spec.clutchReleaseSeconds.has_value() && *spec.clutchReleaseSeconds >= 0.0f)
    {
        settings.clutchReleaseSeconds = *spec.clutchReleaseSeconds;
    }
    if (spec.engineInertia.has_value() && *spec.engineInertia > 0.0f)
    {
        settings.engineInertia = *spec.engineInertia;
    }
    if (spec.coastRpm.has_value() && *spec.coastRpm > 0.0f && spec.coastTorque.has_value() && *spec.coastTorque > 0.0f)
    {
        settings.engineCoastRpm = *spec.coastRpm;
        settings.engineCoastTorque = *spec.coastTorque;
    }
    if (!spec.aeroWings.empty())
    {
        // The air's drag and downforce come from the car's wings at their base angle; the body's own
        // damping, a stand-in for the drag, goes.
        settings.aeroSurfaces.clear();
        for (const VehicleAeroWing& wing : spec.aeroWings)
        {
            const float area = std::max(wing.chord, 0.0f) * std::max(wing.span, 0.0f);
            VehicleAeroSurface surface;
            surface.position = wing.position;
            surface.dragArea = std::max(EvaluateCurve(wing.dragCurve, wing.angleDegrees), 0.0f) * wing.dragGain * area;
            // A lift coefficient below zero pushes the car down.
            surface.downforceArea = -EvaluateCurve(wing.liftCurve, wing.angleDegrees) * wing.liftGain * area;
            settings.aeroSurfaces.push_back(surface);
        }
        settings.linearDamping = 0.0f;
    }
    // The tyres by wheel: the ones the model loader resolved (the library's), the default compound's for a
    // spec read straight from the game's data, else the figures an import before the library wrote per axle.
    {
        const std::array<std::optional<tyre::TyreSpec>, kVehicleWheelCount> compound = DefaultWheelTyres(dryspec);
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            const bool front = index < 2;
            const std::optional<tyre::TyreSpec>& fitted = dryspec.wheelTyres[index].has_value() ? dryspec.wheelTyres[index] : compound[index];
            if (fitted.has_value())
            {
                settings.tyres[index] = TyreSettingsFromSpec(*fitted, StaticTyreLoad(dryspec, front));
            }
            else if (const std::optional<VehicleTyreSettings>& axle = front ? spec.frontTyres : spec.rearTyres; axle.has_value())
            {
                settings.tyres[index] = *axle;
            }
        }
    }

    if (spec.maxSteerAngleDegrees.has_value() && *spec.maxSteerAngleDegrees > 0.0f)
    {
        settings.maxSteerAngleDegrees = *spec.maxSteerAngleDegrees;
    }
    if (spec.brakeTorquePerWheel.has_value() && *spec.brakeTorquePerWheel > 0.0f)
    {
        settings.maxBrakeTorque = *spec.brakeTorquePerWheel;
    }
    if (spec.frontBrakeShare.has_value())
    {
        settings.frontBrakeShare = std::clamp(*spec.frontBrakeShare, 0.05f, 0.95f);
    }
    if (spec.handBrakeTorquePerWheel.has_value() && *spec.handBrakeTorquePerWheel >= 0.0f)
    {
        settings.maxHandBrakeTorque = *spec.handBrakeTorquePerWheel;
    }
    if (const auto abs = spec.electronics.find("ABS"); abs != spec.electronics.end())
    {
        const auto number = [&](const char* key)
        {
            const auto found = abs->second.find(key);
            return found != abs->second.end() ? found->second : 0.0f;
        };
        if (number("PRESENT") > 0.0f && number("ACTIVE") > 0.0f && number("SLIP_RATIO_LIMIT") > 0.0f)
        {
            settings.absSlipRatioLimit = number("SLIP_RATIO_LIMIT");
            settings.absRateHz = number("RATE_HZ");
        }
    }
    // A car whose data has its electronics has the game's traction control or none, not ours.
    if (!spec.electronics.empty())
    {
        settings.tractionControlGrip = 0.0f;
        settings.tcSlipRatioLimit = 0.0f;
        if (const auto tc = spec.electronics.find("TRACTION_CONTROL"); tc != spec.electronics.end())
        {
            const auto number = [&](const char* key)
            {
                const auto found = tc->second.find(key);
                return found != tc->second.end() ? found->second : 0.0f;
            };
            if (number("PRESENT") > 0.0f && number("ACTIVE") > 0.0f && number("SLIP_RATIO_LIMIT") > 0.0f)
            {
                settings.tcSlipRatioLimit = number("SLIP_RATIO_LIMIT");
                settings.tcMinSpeedKmh = std::max(number("MIN_SPEED_KMH"), 0.0f);
                settings.tcRateHz = number("RATE_HZ");
            }
        }
    }
    if (spec.changeDownSeconds.has_value() && *spec.changeDownSeconds > 0.0f)
    {
        settings.gearSwitchDownSeconds = *spec.changeDownSeconds;
    }
    if (spec.autoCutoffSeconds.has_value() && *spec.autoCutoffSeconds > 0.0f)
    {
        settings.upshiftCutSeconds = *spec.autoCutoffSeconds;
    }
    if (spec.clutchMaxTorque.has_value() && *spec.clutchMaxTorque > 0.0f)
    {
        settings.clutchMaxTorque = *spec.clutchMaxTorque;
    }
    if (spec.suspensionFrequencyHz.has_value() && *spec.suspensionFrequencyHz > 0.0f)
    {
        settings.suspensionFrequencyHz = *spec.suspensionFrequencyHz;
    }
    if (spec.suspensionDamping.has_value() && *spec.suspensionDamping >= 0.0f)
    {
        settings.suspensionDamping = *spec.suspensionDamping;
    }
    if (spec.antiRollBars.has_value())
    {
        settings.antiRollBars = *spec.antiRollBars;
    }
    if (spec.limitedSlipDifferentials.has_value())
    {
        settings.limitedSlipDifferentials = *spec.limitedSlipDifferentials;
        // The game's lock under power is the share of the drive torque the clutch pack takes.
        if (spec.differentialPower.has_value() && *spec.differentialPower > 0.0f)
        {
            settings.limitedSlipLock = std::clamp(*spec.differentialPower, 0.05f, 1.0f);
        }
        if (spec.differentialCoast.has_value())
        {
            settings.limitedSlipCoast = std::clamp(*spec.differentialCoast, 0.0f, 1.0f);
        }
        if (spec.differentialPreload.has_value())
        {
            settings.limitedSlipPreload = std::max(*spec.differentialPreload, 0.0f);
        }
    }
    // A car's drive says how its four wheels share the torque; one without four-wheel-drive figures
    // leaves the tuning's centre and axles alone only when it says nothing about its drive.
    if (spec.drive.has_value())
    {
        settings.centreDrive = VehicleCentreDrive::Differential;
        settings.frontTorqueShare = 0.5f;
        settings.centreCouplingRampTorque = 0.0f;
        settings.centreCouplingMaxTorque = 0.0f;
        settings.axleDifferentials = {};
        if (*spec.drive == VehicleDrive::AllWheel && spec.allWheelDrive.has_value())
        {
            const VehicleAllWheelDrive& awd = *spec.allWheelDrive;
            if (awd.coupling && awd.centreRampTorque > 0.0f && awd.centreMaxTorque > 0.0f)
            {
                settings.centreDrive = VehicleCentreDrive::Coupling;
                settings.centreCouplingRampTorque = awd.centreRampTorque;
                settings.centreCouplingMaxTorque = awd.centreMaxTorque;
            }
            else if (!awd.coupling)
            {
                settings.frontTorqueShare = std::clamp(awd.frontShare, 0.0f, 1.0f);
            }
            settings.axleDifferentials[0] = VehicleAxleDifferential{
                std::clamp(awd.frontDiffPower, 0.0f, 1.0f), std::max(awd.frontDiffPreload, 0.0f), std::clamp(awd.frontDiffCoast, 0.0f, 1.0f)};
            settings.axleDifferentials[1] = VehicleAxleDifferential{
                std::clamp(awd.rearDiffPower, 0.0f, 1.0f), std::max(awd.rearDiffPreload, 0.0f), std::clamp(awd.rearDiffCoast, 0.0f, 1.0f)};
        }
    }
    // A car's own data (its mass says it has some) brings its rear steering and its body, or none.
    if (spec.massKg.has_value())
    {
        settings.rearSteerControllers = spec.rearSteerControllers;
        settings.carColliders = spec.colliders;
    }
    if (spec.steeringWheelLockDegrees.has_value() && *spec.steeringWheelLockDegrees > 0.0f)
    {
        settings.steeringWheelLockDegrees = *spec.steeringWheelLockDegrees;
    }
    // The linkage only when both axles have one: half a multibody car would not drive.
    if (spec.frontSuspension.has_value() && spec.rearSuspension.has_value() && spec.frontSuspension->type != VehicleSuspensionType::None && spec.rearSuspension->type != VehicleSuspensionType::None)
    {
        settings.frontSuspension = *spec.frontSuspension;
        settings.rearSuspension = *spec.rearSuspension;
    }
    return settings;
}

VehicleWheelGeometry GetVehicleWheelMount(const VehicleSettings& settings, size_t index)
{
    const bool left = index % 2 == 0;
    const bool front = index < 2;
    VehicleWheelGeometry mount;
    mount.radius = settings.wheelRadius;
    mount.width = settings.wheelWidth;
    if (settings.hasWheelLayout && index < kVehicleWheelCount)
    {
        const VehicleWheelGeometry& wheel = settings.wheelLayout[index];
        mount = wheel;
        // The centre rests the spring's rest length below the mount.
        mount.center.y += ComputeRestSuspensionLength(settings, kGravity);
        return mount;
    }
    mount.center = glm::vec3(
        settings.trackCenterX + (left ? settings.halfTrackWidth : -settings.halfTrackWidth),
        settings.wheelMountY,
        front ? settings.frontAxleZ : settings.rearAxleZ);
    return mount;
}

float ComputeRestSuspensionLength(const VehicleSettings& settings, float gravity)
{
    // The spring is a harmonic oscillator of the wheel's share of the mass, so its static deflection
    // is g / omega^2 whatever that share.
    const float omega = 2.0f * std::numbers::pi_v<float> * std::max(settings.suspensionFrequencyHz, 0.01f);
    const float deflection = gravity / (omega * omega);
    return std::clamp(
        settings.suspensionMaxLength - deflection,
        settings.suspensionMinLength,
        settings.suspensionMaxLength);
}

VehicleDriverInput ResolveVehicleDriverInput(const VehicleControls& controls, float forwardSpeed, float& direction)
{
    if (direction == 0.0f)
    {
        direction = 1.0f;
    }

    VehicleDriverInput input;
    input.right = std::clamp(controls.steering, -1.0f, 1.0f);
    input.brake = std::clamp(controls.brake, 0.0f, 1.0f);
    input.handBrake = std::clamp(controls.handBrake, 0.0f, 1.0f);

    const float throttle = std::clamp(controls.throttle, -1.0f, 1.0f);
    if (throttle * direction < 0.0f)
    {
        // Asking for the other direction: brake while the car still rolls the current way.
        if (forwardSpeed * direction > kDirectionChangeSpeed)
        {
            input.brake = std::max(input.brake, std::abs(throttle));
        }
        else
        {
            direction = throttle > 0.0f ? 1.0f : -1.0f;
            input.forward = throttle;
        }
    }
    else
    {
        input.forward = throttle;
    }

    if (input.handBrake > 0.0f)
    {
        input.forward = 0.0f;
    }
    return input;
}

VehicleDriverInput ResolveManualDriverInput(const VehicleControls& controls, int gear)
{
    VehicleDriverInput input;
    input.right = std::clamp(controls.steering, -1.0f, 1.0f);
    input.handBrake = std::clamp(controls.handBrake, 0.0f, 1.0f);
    const float throttle = std::clamp(controls.throttle, -1.0f, 1.0f);
    input.brake = std::max(std::clamp(controls.brake, 0.0f, 1.0f), -throttle);
    input.forward = std::max(throttle, 0.0f) * (gear < 0 ? -1.0f : 1.0f);
    return input;
}

VehicleShiftPoints ComputeVehicleShiftPoints(const VehicleSettings& settings)
{
    // Along the rev range from the idle to the limiter: full throttle changes up just under the
    // limiter and kicks down while the lower gear still has room to pull; a light foot changes up
    // early and the box holds a gear off the throttle until the engine nears the idle.
    const float idle = std::max(settings.minRpm, 1.0f);
    const float range = std::max(settings.maxRpm - idle, 1.0f);
    const float limiter = idle + range;
    VehicleShiftPoints points;
    points.upFull = settings.shiftUpRpm > 0.0f ? std::min(settings.shiftUpRpm, limiter) : idle + 0.93f * range;
    points.downClosed = settings.shiftDownRpm > 0.0f ? settings.shiftDownRpm : idle + 0.12f * range;
    const float lowest = idle + 0.05f * range;
    points.downClosed = std::min(std::max(points.downClosed, lowest), std::max(points.upFull - 0.3f * range, lowest));
    points.upLight = std::min(std::max(idle + 0.35f * range, points.downClosed + 0.15f * range), points.upFull);
    points.downFull = std::max(std::min(idle + 0.4f * range, points.upFull - 0.35f * range), points.downClosed);
    return points;
}

float VehicleGearRpm(const VehicleGearbox& gearbox, int gear, float outputRpm)
{
    if (gear < 0)
    {
        return std::abs(outputRpm * gearbox.reverseRatio);
    }
    if (gear == 0 || gear > static_cast<int>(gearbox.forwardRatios.size()))
    {
        return 0.0f;
    }
    return std::abs(outputRpm * gearbox.forwardRatios[static_cast<size_t>(gear - 1)]);
}

namespace
{
// Starts a change into the gear the state now holds. Under load the clutch opens while the gears change
// (a change down has its own time), then bites; between gears with no torque through them (from rest,
// or rolling below the idle) the box just selects it. A change up under load also cuts the engine for
// the cut time.
void StartGearChange(const VehicleGearbox& gearbox, VehicleGearboxState& state, bool underLoad, bool up = false)
{
    const float switchSeconds = !up && gearbox.switchDownSeconds > 0.0f ? gearbox.switchDownSeconds : gearbox.switchSeconds;
    state.switchLeft = underLoad ? switchSeconds : 0.0f;
    state.cutLeft = underLoad && up ? gearbox.upshiftCutSeconds : 0.0f;
    state.releaseLeft = gearbox.releaseSeconds;
    state.latencyLeft = gearbox.latencySeconds;
    state.launching = false;
}

// Moving off in a forward gear, a launch rpm slips the clutch on the engine's revs instead.
void StartLaunch(const VehicleGearbox& gearbox, VehicleGearboxState& state, float engineRpm)
{
    if (gearbox.launchRpm > 0.0f && state.gear > 0 && engineRpm > 0.0f)
    {
        state.launching = true;
        state.releaseLeft = 0.0f;
    }
}

// The clutch for the gear the box is in: open off the throttle below the idle (or while `declutch`
// holds it), slipping on a launch, open through a change and biting after it.
void WorkGearboxClutch(const VehicleGearbox& gearbox, VehicleGearboxState& state, float throttle, float outputRpm, float deltaSeconds,
                       float engineRpm, bool declutch)
{
    // Off the throttle and too slow for the gear to turn the engine at its idle, the clutch opens: the
    // car rolls to a stop with the engine idling rather than the engine pushing it on.
    const bool idle = declutch || (throttle <= 0.0f && VehicleGearRpm(gearbox, state.gear, outputRpm) < gearbox.idleRpm);
    if (idle)
    {
        state.idling = true;
        state.switchLeft = 0.0f;
        state.cutLeft = 0.0f;
        state.releaseLeft = 0.0f;
        state.latencyLeft = 0.0f;
        state.clutch = 0.0f;
        state.revMatch = false;
        state.launching = false;
        return;
    }
    if (state.idling)
    {
        state.idling = false;
        StartGearChange(gearbox, state, false);
        StartLaunch(gearbox, state, engineRpm);
    }

    if (state.launching)
    {
        // The clutch closes as the engine's revs pass the launch point (throttle-scaled from the idle):
        // the engine settles where the clutch passes what it makes. The launch is over once the wheels
        // turn the engine at its own speed.
        const float gearRpm = VehicleGearRpm(gearbox, state.gear, outputRpm);
        const float idleRpm = std::max(gearbox.idleRpm, 1.0f);
        const float launch = idleRpm + (std::max(gearbox.launchRpm, idleRpm) - idleRpm) * throttle;
        const float low = 0.95f * launch;
        const float high = 1.15f * launch;
        const float x = std::clamp((engineRpm - low) / std::max(high - low, 1.0f), 0.0f, 1.0f);
        state.clutch = x * x * (3.0f - 2.0f * x);
        state.revMatch = false;
        state.latencyLeft = std::max(state.latencyLeft - deltaSeconds, 0.0f);
        // Over once the clutch is shut (what slip is left is traction control's) or the wheels turn the
        // engine at its own speed.
        if (x >= 1.0f || (gearRpm >= 0.98f * engineRpm && gearRpm >= low))
        {
            state.launching = false;
            state.clutch = 1.0f;
        }
        return;
    }

    state.revMatch = state.switchLeft > 0.0f;
    state.cutLeft = std::max(state.cutLeft - deltaSeconds, 0.0f);
    if (state.switchLeft > 0.0f)
    {
        state.switchLeft = std::max(state.switchLeft - deltaSeconds, 0.0f);
        state.clutch = 0.0f;
    }
    else if (state.releaseLeft > 0.0f)
    {
        state.releaseLeft = std::max(state.releaseLeft - deltaSeconds, 0.0f);
        state.clutch = gearbox.releaseSeconds > 0.0f ? 1.0f - state.releaseLeft / gearbox.releaseSeconds : 1.0f;
    }
    else
    {
        state.clutch = 1.0f;
        state.latencyLeft = std::max(state.latencyLeft - deltaSeconds, 0.0f);
    }
}
}

void UpdateAutomaticGearbox(const VehicleGearbox& gearbox, VehicleGearboxState& state, float forward, float outputRpm, float deltaSeconds, float engineRpm,
                            int shifts)
{
    const int top = static_cast<int>(gearbox.forwardRatios.size());
    const float throttle = std::clamp(std::abs(forward), 0.0f, 1.0f);
    const auto gearRpm = [&](int gear)
    {
        return VehicleGearRpm(gearbox, gear, outputRpm);
    };

    // Drive or reverse, as the throttle asks; the caller only asks once the car has stopped.
    if ((forward > 0.0f && state.gear < 0) || (forward < 0.0f && state.gear > 0) || state.gear == 0)
    {
        state.gear = forward < 0.0f ? -1 : 1;
        state.manualHoldLeft = 0.0f;
        StartGearChange(gearbox, state, false);
        StartLaunch(gearbox, state, engineRpm);
    }

    // The driver's changes (the paddles), forward only: the box holds the gear for a while after each.
    state.manualHoldLeft = std::max(state.manualHoldLeft - deltaSeconds, 0.0f);
    if (shifts != 0 && state.gear > 0 && top > 0)
    {
        state.manualHoldLeft = gearbox.manualHoldSeconds;
        for (; shifts != 0; shifts += shifts > 0 ? -1 : 1)
        {
            const int from = state.gear;
            const int to = std::clamp(from + (shifts > 0 ? 1 : -1), 1, top);
            if (to == from || (to < from && gearbox.limiterRpm > 0.0f && gearRpm(to) > gearbox.limiterRpm))
            {
                break; // past first or top, or a change down onto the limiter
            }
            state.gear = to;
            if (!state.idling)
            {
                StartGearChange(gearbox, state, true, to > from);
            }
        }
    }

    const bool ready = state.idling || (state.switchLeft <= 0.0f && state.releaseLeft <= 0.0f && state.latencyLeft <= 0.0f);
    if (state.manualHoldLeft > 0.0f && state.gear > 0 && top > 0)
    {
        // Holding the driver's gear: up only on the limiter (by the engine's revs too, which run ahead
        // of the wheels while the clutch slips), down only where the engine would labour.
        if (ready)
        {
            const float limiter = gearbox.limiterRpm > 0.0f ? 0.99f * gearbox.limiterRpm : gearbox.shiftPoints.upFull;
            const float revs = std::max(gearRpm(state.gear), state.launching ? 0.0f : engineRpm);
            int target = std::min(state.gear, top);
            if (target < top && revs >= limiter)
            {
                ++target;
            }
            else if (target > 1 && gearRpm(target) < gearbox.shiftPoints.downClosed)
            {
                --target;
            }
            if (target != state.gear)
            {
                const bool up = target > state.gear;
                state.gear = target;
                if (!state.idling)
                {
                    StartGearChange(gearbox, state, true, up);
                }
            }
        }
    }
    else if (state.gear > 0 && top > 0 && ready)
    {
        const VehicleShiftPoints& points = gearbox.shiftPoints;
        const float up = points.upLight + (points.upFull - points.upLight) * throttle;
        const float down = points.downClosed + (points.downFull - points.downClosed) * throttle;
        int target = std::min(state.gear, top);
        // The engine may run ahead of the wheels while the clutch slips (traction control): its own
        // revs bring the first change up on, or it would sit on the limiter while the wheels never
        // reach the point.
        const float slipping = !state.launching && target == state.gear ? std::max(engineRpm - gearRpm(target), 0.0f) : 0.0f;
        if (gearRpm(target) + slipping > up)
        {
            // Up while the engine is past the point, as long as the next gear keeps it clear of the
            // point it would change back down at.
            while (target < top && gearRpm(target) + slipping > up && gearRpm(target + 1) > down * 1.05f)
            {
                ++target;
            }
        }
        else if (gearRpm(target) < down)
        {
            // Down as far as the speed allows without the lower gear passing the point it would change back up at.
            while (target > 1 && gearRpm(target) < down && gearRpm(target - 1) < up * 0.9f)
            {
                --target;
            }
        }
        if (target != state.gear)
        {
            const bool up = target > state.gear;
            state.gear = target;
            if (!state.idling)
            {
                StartGearChange(gearbox, state, true, up);
            }
        }
    }

    WorkGearboxClutch(gearbox, state, throttle, outputRpm, deltaSeconds, engineRpm, false);
}

void UpdateManualGearbox(const VehicleGearbox& gearbox, VehicleGearboxState& state, int shifts, float forward, float outputRpm,
                         float deltaSeconds, float engineRpm, bool declutch)
{
    const int top = static_cast<int>(gearbox.forwardRatios.size());
    const float throttle = std::clamp(std::abs(forward), 0.0f, 1.0f);
    const int lowest = gearbox.reverseRatio > 0.0f ? -1 : 0;
    state.manualHoldLeft = 0.0f; // the automatic's hold, should the driver switch back to it
    for (; shifts != 0; shifts += shifts > 0 ? -1 : 1)
    {
        const int from = state.gear;
        const int to = std::clamp(from + (shifts > 0 ? 1 : -1), lowest, top);
        const float toRpm = VehicleGearRpm(gearbox, to, outputRpm);
        if (to == from || (to > 0 && to < from && gearbox.limiterRpm > 0.0f && toRpm > gearbox.limiterRpm) ||
            (to < 0 && toRpm > gearbox.idleRpm))
        {
            break; // past the top or reverse, a change down onto the limiter, or reverse while still rolling
        }
        state.gear = to;
        if (to == 0)
        {
            continue;
        }
        if (state.idling)
        {
            // Out of neutral, or with the clutch already open: it bites as it leaves the idle (WorkGearboxClutch).
            continue;
        }
        StartGearChange(gearbox, state, true, to > from);
    }
    if (state.gear == 0)
    {
        // Neutral: nothing through the clutch; in gear again, it bites (WorkGearboxClutch leaving the idle).
        state.idling = true;
        state.clutch = 0.0f;
        state.switchLeft = 0.0f;
        state.cutLeft = 0.0f;
        state.releaseLeft = 0.0f;
        state.latencyLeft = 0.0f;
        state.revMatch = false;
        state.launching = false;
        return;
    }
    const float gearRpm = VehicleGearRpm(gearbox, state.gear, outputRpm);
    if (state.idling && !declutch && gearRpm >= gearbox.idleRpm && engineRpm < gearRpm)
    {
        // Into gear while rolling faster than the engine turns: the revs come up to the gear's over the
        // change rather than the clutch snatching at them.
        state.idling = false;
        StartGearChange(gearbox, state, true);
    }
    WorkGearboxClutch(gearbox, state, throttle, outputRpm, deltaSeconds, engineRpm, declutch);
}
}
