#include "vehicle_steering_assist.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me
{

namespace
{
constexpr float kGravity = 9.81f;
constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
// The body's slip angle is read only above this forward speed (m/s), and in full from twice it: at a
// crawl the velocity's direction says nothing.
constexpr float kSlipMinSpeed = 1.5f;

float MoveTowards(float value, float target, float maxDelta)
{
    if (std::abs(target - value) <= maxDelta)
    {
        return target;
    }
    return value + (target > value ? maxDelta : -maxDelta);
}

// The share of a step a first-order filter with this time constant moves towards its target.
float Blend(float seconds, float deltaSeconds)
{
    return seconds > 0.0f ? 1.0f - std::exp(-deltaSeconds / seconds) : 1.0f;
}
}

float ApplySteeringResponse(float request, float sensitivity)
{
    const float x = std::clamp(request, -1.0f, 1.0f);
    const float s = std::clamp(sensitivity, 0.0f, 1.0f);
    return x * (1.0f - s) + x * x * x * s;
}

float ComputeSteeringLockShare(const VehicleSteeringAssistSettings& settings, const VehicleSteeringAssistInput& input)
{
    if (!settings.speedSensitive || input.maxSteerDegrees <= 0.0f)
    {
        return 1.0f;
    }
    // The steady corner at `cornerGrip` g needs the wheels at about wheelbase / radius, radius = v^2 / a;
    // the tyres' peak slip angle on top of that is the most that still adds grip.
    const float speed = std::max(std::abs(input.forwardSpeed), std::max(settings.fullLockSpeed, 0.1f));
    const float corner = std::atan(std::max(input.wheelbase, 0.0f) * std::max(settings.cornerGrip, 0.0f) * kGravity / (speed * speed));
    const float limit = corner + std::max(input.peakSlipDegrees, 0.0f) * kRadiansPerDegree;
    const float share = limit / (input.maxSteerDegrees * kRadiansPerDegree);
    return std::clamp(share, std::clamp(settings.minLockShare, 0.0f, 1.0f), 1.0f);
}

float ComputeAssistedSteering(
    const VehicleSteeringAssistSettings& settings, const VehicleSteeringAssistInput& input, VehicleSteeringAssistState& state, float deltaSeconds)
{
    const float request = std::clamp(input.request, -1.0f, 1.0f);
    if (!settings.enabled)
    {
        state = VehicleSteeringAssistState{request, request, 0.0f};
        return request;
    }
    const float dt = std::max(deltaSeconds, 0.0f);

    // Away from the centre at the steering rate, back towards it (or across it) at the faster return rate.
    const float target = ApplySteeringResponse(request, settings.sensitivity);
    const bool outward = target * state.limited >= 0.0f && std::abs(target) > std::abs(state.limited);
    const float seconds = outward ? settings.steerSeconds : settings.returnSeconds;
    state.limited = seconds > 0.0f ? MoveTowards(state.limited, target, dt / seconds) : target;
    state.request += (state.limited - state.request) * Blend(settings.smoothingSeconds, dt);

    // Where the car is going: the wheels pointed along the body's velocity carry no slip.
    float centreTarget = 0.0f;
    if (settings.counterSteerAssist && input.forwardSpeed > kSlipMinSpeed && input.maxSteerDegrees > 0.0f)
    {
        const float slip = std::atan2(input.rightSpeed, input.forwardSpeed);
        const float deadZone = std::max(settings.counterSteerDeadZoneDegrees, 0.0f) * kRadiansPerDegree;
        const float beyond = std::copysign(std::max(std::abs(slip) - deadZone, 0.0f), slip);
        const float weight = std::clamp(input.forwardSpeed / kSlipMinSpeed - 1.0f, 0.0f, 1.0f);
        centreTarget = std::clamp(weight * beyond / (input.maxSteerDegrees * kRadiansPerDegree), -1.0f, 1.0f);
    }
    state.centre += (centreTarget - state.centre) * Blend(settings.smoothingSeconds * 2.0f, dt);

    return std::clamp(state.centre + state.request * ComputeSteeringLockShare(settings, input), -1.0f, 1.0f);
}
}
