#include <engine/editor/services/vehicle_steering_assist.h>

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
    VehicleSteeringAssistState state;
    const float handsOff = Hold(settings, input, state, 1.0f);
    Require(std::abs(handsOff - (-18.0f / 35.0f)) < 0.01f, "hands off, the wheels point along the slide less the dead zone");

    settings.counterSteerAssist = false;
    state = VehicleSteeringAssistState{};
    Require(std::abs(Hold(settings, input, state, 1.0f)) < 1e-3f, "without the assist the wheels stay straight");
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
