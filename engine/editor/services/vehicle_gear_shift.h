#pragma once

#include <engine/asset/model_loader.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace me
{

// A gear change as the driver's hand and the gear lever make it, after the gearbox has made it: the
// hand goes from the wheel to the knob, the lever goes through the H-gate from the old gear to the
// new one by way of neutral, and the hand goes back to the wheel.
struct VehicleGearShift
{
    int from = 0;
    int to = 0;
    // Since the change began; past kGearShiftSeconds it is over.
    float seconds = 1e3f;
};

inline constexpr float kGearShiftReachSeconds = 0.12f;
inline constexpr float kGearShiftMoveSeconds = 0.2f;
inline constexpr float kGearShiftReturnSeconds = 0.18f;
inline constexpr float kGearShiftSeconds = kGearShiftReachSeconds + kGearShiftMoveSeconds + kGearShiftReturnSeconds;

// Where a gear sits in a six-speed H-gate, in gate units: x across the gate to the car's left (1-2 one
// plane left of neutral's, 3-4 neutral's, 5-6 one right, reverse left of 1-2), y forward (odd gears
// forward, even back). Neutral is the middle.
glm::vec2 GearGatePosition(int gear);

// Starts a change to `gear` from where the lever is going; one made while the hand is still at the lever
// keeps the hand there.
void StartGearShift(VehicleGearShift& shift, int gear);
void AdvanceGearShift(VehicleGearShift& shift, float deltaSeconds);

// Where the lever is in the gate now (gate units): at the old gear while the hand reaches for it, along
// the gate's lines to neutral, across and into the new gear, then there.
glm::vec2 GearLeverGatePosition(const VehicleGearShift& shift);
// How far the hand is from the wheel to the knob: 0 on the wheel, 1 on the knob, eased.
float GearShiftHandWeight(const VehicleGearShift& shift);

// The lever turned for its knob to sit at `gate`: a turn about the pivot, in the model's space, for a
// car whose vehicle space turns into its model's by vehicleToModel. A gate unit moves the knob 45 mm
// across and 60 mm forward.
glm::mat4 GearLeverTransform(const ModelGearLever& lever, const glm::vec2& gate, const glm::quat& vehicleToModel);
}
