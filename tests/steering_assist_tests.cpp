#include "gtr_car_spec.h"

#include <engine/editor/services/vehicle_steering_assist.h>
#include <engine/physics/physics_world.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

constexpr float kFrame = 1.0f / 60.0f;

// Runs the assist for `seconds` at a steady input and returns its last output.
float Hold(const VehicleSteeringAssistSettings& settings, const VehicleSteeringAssistInput& input, VehicleSteeringAssistState& state, float seconds)
{
    float steering = 0.0f;
    for (float time = 0.0f; time < seconds; time += kFrame)
    {
        steering = ComputeAssistedSteering(settings, input, state, kFrame);
    }
    return steering;
}

void ResponseCurveKeepsTheEnds()
{
    Require(ApplySteeringResponse(1.0f, 0.7f) == 1.0f && ApplySteeringResponse(-1.0f, 0.7f) == -1.0f, "full deflection stays full");
    Require(ApplySteeringResponse(0.5f, 0.0f) == 0.5f, "no curve is linear");
    Require(ApplySteeringResponse(0.3f, 1.0f) < 0.03f, "a cubic curve is fine near the centre");
}

void SteeringFollowsAtALimitedRate()
{
    VehicleSteeringAssistSettings settings;
    settings.counterSteerAssist = false;
    VehicleSteeringAssistInput input;
    input.request = 1.0f;
    VehicleSteeringAssistState state;
    const float first = ComputeAssistedSteering(settings, input, state, kFrame);
    Require(first > 0.0f && first < 0.1f, "a full flick does not reach full lock in one frame");
    const float later = Hold(settings, input, state, 1.0f);
    Require(std::abs(later - 1.0f) < 1e-3f, "standing still, the stick reaches full lock");

    input.request = 0.0f;
    const float released = Hold(settings, input, state, 0.5f);
    Require(std::abs(released) < 1e-3f, "let go, the steering is back at the centre within the return time");
}

void LockShrinksWithSpeed()
{
    VehicleSteeringAssistSettings settings;
    VehicleSteeringAssistInput input;
    input.maxSteerDegrees = 35.0f;
    input.wheelbase = 2.665f;
    input.peakSlipDegrees = 7.0f;
    input.forwardSpeed = 2.0f;
    Require(ComputeSteeringLockShare(settings, input) == 1.0f, "at a crawl the full lock is there");
    input.forwardSpeed = 100.0f / 3.6f;
    const float fast = ComputeSteeringLockShare(settings, input);
    // 2.665 * 9.81 / 27.78^2 = 0.0339 rad (1.94 deg), plus 7 deg of slip: 8.94 of 35 degrees.
    Require(std::abs(fast - 8.94f / 35.0f) < 0.01f, "at 100 km/h full deflection is the corner's angle plus the peak slip");
    input.forwardSpeed = 300.0f / 3.6f;
    Require(ComputeSteeringLockShare(settings, input) < fast, "faster, less lock");
    settings.speedSensitive = false;
    Require(ComputeSteeringLockShare(settings, input) == 1.0f, "off, the full lock is there at any speed");
}

void SlideCentresTheRange()
{
    VehicleSteeringAssistSettings settings;
    settings.speedSensitive = false;
    VehicleSteeringAssistInput input;
    input.maxSteerDegrees = 35.0f;
    input.forwardSpeed = 20.0f;
    // The car slides with its velocity 20 degrees left of its nose (the tail out to the right).
    input.rightSpeed = -20.0f * std::tan(20.0f * 3.14159265f / 180.0f);
    // Not turning, the front axle goes the body's way.
    input.frontRightSpeed = input.rightSpeed;
    VehicleSteeringAssistState state;
    const float handsOff = Hold(settings, input, state, 1.0f);
    Require(std::abs(handsOff - (-18.0f / 35.0f)) < 0.01f, "hands off, the wheels point along the slide less the dead zone");

    settings.counterSteerAssist = false;
    state = VehicleSteeringAssistState{};
    Require(std::abs(Hold(settings, input, state, 1.0f)) < 1e-3f, "without the assist the wheels stay straight");
}

// The front wheels' angle (degrees) the assist steers to with the stick held at `request`.
float HeldWheelDegrees(const VehicleSteeringAssistSettings& settings, VehicleSteeringAssistInput input, float request)
{
    input.request = request;
    VehicleSteeringAssistState state;
    return Hold(settings, input, state, 1.0f) * input.maxSteerDegrees;
}

void FrontWheelsStayWithinThePeakSlip()
{
    VehicleSteeringAssistSettings settings;
    settings.slipLimitShare = 1.0f;
    settings.sensitivity = 0.0f;
    settings.speedSensitive = false;
    settings.counterSteerAssist = false;
    VehicleSteeringAssistInput input;
    input.maxSteerDegrees = 35.0f;
    input.peakSlipDegrees = 7.0f;
    input.forwardSpeed = 30.0f;
    // Turning in, the front axle still goes straight on: the wheels get the peak slip and no more.
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 7.0f) < 0.05f, "full stick going straight is the peak slip");
    Require(std::abs(HeldWheelDegrees(settings, input, -1.0f) + 7.0f) < 0.05f, "and the same to the left");
    Require(std::abs(HeldWheelDegrees(settings, input, 0.1f) - 3.5f) < 0.05f, "inside the window the stick is untouched");

    // The car yaws and the front axle turns 4 degrees to the right: the window goes with it.
    input.frontRightSpeed = 30.0f * std::tan(4.0f * 3.14159265f / 180.0f);
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 11.0f) < 0.05f, "the peak slip on top of the front axle's turn");
    Require(std::abs(HeldWheelDegrees(settings, input, -1.0f) + 3.0f) < 0.05f, "and no more than the peak slip the other way");

    // The front axle swings 10 degrees left: steering right would only scrub, but the limit takes lock
    // away and never turns the wheels the other way.
    input.frontRightSpeed = -30.0f * std::tan(10.0f * 3.14159265f / 180.0f);
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f)) < 0.05f, "against the slide the wheels go no further than straight");
    Require(std::abs(HeldWheelDegrees(settings, input, 0.0f)) < 0.05f, "hands off they stay straight");
    Require(std::abs(HeldWheelDegrees(settings, input, -1.0f) + 17.0f) < 0.05f, "into the slide up to the peak slip past it");
    input.frontRightSpeed = 30.0f * std::tan(4.0f * 3.14159265f / 180.0f);

    settings.slipLimitShare = 0.5f;
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 7.5f) < 0.05f, "the share scales the slip allowed");

    settings.slipLimit = false;
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 35.0f) < 0.05f, "off, the full lock is there");
}

void SlipLimitFadesInAboveFullLockSpeed()
{
    VehicleSteeringAssistSettings settings;
    settings.slipLimitShare = 1.0f;
    settings.speedSensitive = false;
    settings.counterSteerAssist = false;
    VehicleSteeringAssistInput input;
    input.maxSteerDegrees = 35.0f;
    input.peakSlipDegrees = 7.0f;
    input.forwardSpeed = settings.fullLockSpeed;
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 35.0f) < 0.05f, "at the full-lock speed the full lock is there");
    input.forwardSpeed = 1.5f * settings.fullLockSpeed;
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 21.0f) < 0.05f, "half way, half the limit");
    input.forwardSpeed = -10.0f;
    Require(std::abs(HeldWheelDegrees(settings, input, 1.0f) - 35.0f) < 0.05f, "reversing, no limit");
}

// The GT-R (the GT3 car's data) on its own wheels' places, as the vehicle physics tests have it.
VehicleSettings GtrSettings()
{
    VehicleWheelLayout layout{};
    const float frontZ = 2.78f * (1.0f - 0.555f);
    const float rearZ = frontZ - 2.78f;
    layout[0] = {glm::vec3(0.8375f, 0.355f, frontZ), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8375f, 0.355f, frontZ), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.84f, 0.355f, rearZ), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.84f, 0.355f, rearZ), 0.355f, 0.33f};
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, test::MakeGtrSpec());
    tuning.wheelRadius = 0.355f;
    VehicleSettings settings = FitVehicleSettingsToBounds(glm::vec3(-1.0f, 0.0f, -2.3f), glm::vec3(1.0f, 1.2f, 2.3f), tuning, &layout);
    settings.centerOfMassOffset = glm::vec3(0.0f, 0.38f - settings.chassisCenter.y, -settings.chassisCenter.z);
    return settings;
}

struct StepSteer
{
    // Over the turn: the front tyres' slip angle (degrees) and the lateral acceleration (g, the yaw rate
    // times the speed) on average, and the speed it scrubbed off (km/h).
    float frontSlip = 0.0f;
    float lateralG = 0.0f;
    float speedLost = 0.0f;
    // The most the front tyres' own slip angle and the wheels' angle less the front axle's travel differ
    // (degrees): the telemetry the limit reads, against the tyres.
    float travelError = 0.0f;
};

// The car run up to `kmh` on flat ground, then the stick held full right through the assist for two
// seconds on a part throttle; the first half second (the wheels turning in) is not counted.
StepSteer RunStepSteer(const VehicleSettings& car, const VehicleSteeringAssistSettings& settings, float kmh)
{
    PhysicsWorld world;
    world.AddStaticBox(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(500.0f, 0.5f, 500.0f));
    const VehicleId id = world.AddVehicle(car, {glm::vec3(0.0f, 0.3f, -450.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(id, controls);
    for (int frame = 0; frame < 60 * 60 && world.GetVehicleTelemetry(id).forwardSpeed < kmh / 3.6f; ++frame)
    {
        world.Update(kFrame);
    }
    controls.throttle = 0.3f;
    const float startSpeed = world.GetVehicleTelemetry(id).forwardSpeed;
    VehicleSteeringAssistInput input;
    input.request = 1.0f;
    input.maxSteerDegrees = car.maxSteerAngleDegrees;
    input.wheelbase = car.frontAxleZ - car.rearAxleZ;
    input.peakSlipDegrees = car.tyres[0].peakSlipAngleDegrees;
    VehicleSteeringAssistState state;
    StepSteer result;
    int counted = 0;
    for (int frame = 0; frame < 120; ++frame)
    {
        const VehicleTelemetry telemetry = world.GetVehicleTelemetry(id);
        input.forwardSpeed = telemetry.forwardSpeed;
        input.rightSpeed = telemetry.rightSpeed;
        input.frontRightSpeed = telemetry.frontAxleRightSpeed;
        controls.steering = ComputeAssistedSteering(settings, input, state, kFrame);
        world.SetVehicleControls(id, controls);
        const glm::quat before = world.GetVehiclePose(id).rotation;
        world.Update(kFrame);
        if (frame < 30)
        {
            continue;
        }
        const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(id);
        const float slip = 0.5f * (std::abs(wheels[0].slipAngleDegrees) + std::abs(wheels[1].slipAngleDegrees));
        const VehicleTelemetry now = world.GetVehicleTelemetry(id);
        const float yawRate = std::abs(glm::eulerAngles(glm::conjugate(before) * world.GetVehiclePose(id).rotation).y) / kFrame;
        const float travel = std::atan2(now.frontAxleRightSpeed, now.forwardSpeed) * 180.0f / 3.14159265f;
        result.frontSlip += slip;
        result.lateralG += yawRate * std::hypot(now.forwardSpeed, now.rightSpeed) / 9.81f;
        result.travelError = std::max(result.travelError, std::abs(slip - std::abs(controls.steering * car.maxSteerAngleDegrees - travel)));
        ++counted;
    }
    result.frontSlip /= static_cast<float>(counted);
    result.lateralG /= static_cast<float>(counted);
    result.speedLost = (startSpeed - world.GetVehicleTelemetry(id).forwardSpeed) * 3.6f;
    return result;
}

// The GT-R pushing: full stick at 60 km/h. With the speed-sensitive lock alone the front tyres run well
// past their peak and scrub the speed off; with the slip limit they stay near it and the car corners as
// hard or harder.
void SlipLimitHoldsTheFrontTyresAtTheirPeak()
{
    const VehicleSettings car = GtrSettings();
    VehicleSteeringAssistSettings open;
    open.slipLimit = false;
    const VehicleSteeringAssistSettings closed;
    const StepSteer before = RunStepSteer(car, open, 60.0f);
    const StepSteer after = RunStepSteer(car, closed, 60.0f);
    std::cout << "GT-R full stick at 60 km/h, without -> with the slip limit: front slip " << before.frontSlip << " -> " << after.frontSlip << " deg (peak "
              << car.tyres[0].peakSlipAngleDegrees << "), lateral " << before.lateralG << " -> " << after.lateralG << " g, speed lost " << before.speedLost << " -> "
              << after.speedLost << " km/h, telemetry against the tyres within " << after.travelError << " deg\n";
    const float peak = car.tyres[0].peakSlipAngleDegrees;
    Require(after.travelError < 1.0f, "the front axle's travel and the wheels' angle give the tyres' slip");
    Require(before.frontSlip > 1.25f * peak, "without the limit the front tyres are well past their peak");
    Require(after.frontSlip < closed.slipLimitShare * peak + 0.5f, "with it they stay at the share of the peak");
    Require(after.lateralG > 0.98f * before.lateralG, "and the car corners as hard");
    Require(after.speedLost < before.speedLost, "with less speed scrubbed off");
}

void DisabledPassesTheRequest()
{
    VehicleSteeringAssistSettings settings;
    settings.enabled = false;
    VehicleSteeringAssistInput input;
    input.request = 0.6f;
    input.forwardSpeed = 50.0f;
    input.rightSpeed = 10.0f;
    VehicleSteeringAssistState state;
    Require(ComputeAssistedSteering(settings, input, state, kFrame) == 0.6f, "off, the stick drives the rack");
}
}

int main()
{
    try
    {
        ResponseCurveKeepsTheEnds();
        SteeringFollowsAtALimitedRate();
        LockShrinksWithSpeed();
        SlideCentresTheRange();
        FrontWheelsStayWithinThePeakSlip();
        SlipLimitFadesInAboveFullLockSpeed();
        SlipLimitHoldsTheFrontTyresAtTheirPeak();
        DisabledPassesTheRequest();
    }
    catch (const std::exception& error)
    {
        std::cerr << "steering assist test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "steering assist tests passed\n";
    return 0;
}
