#include "vehicle_gear_shift.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace me
{

namespace
{
constexpr float kGateAcrossMetres = 0.045f;
constexpr float kGateForwardMetres = 0.06f;

// The turn taking direction `from` to `to` the shortest way.
glm::quat TurnBetween(const glm::vec3& from, const glm::vec3& to)
{
    const glm::vec3 a = glm::normalize(from);
    const glm::vec3 b = glm::normalize(to);
    const float cosine = glm::dot(a, b);
    const glm::vec3 axis = glm::cross(a, b);
    if (cosine < -0.9999f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}
}

glm::vec2 GearGatePosition(int gear)
{
    if (gear < 0)
    {
        // Reverse beyond first, as on the R34's lever with its reverse lock-out ring (assumed).
        return glm::vec2(2.0f, 1.0f);
    }
    if (gear == 0)
    {
        return glm::vec2(0.0f);
    }
    const int plane = (gear - 1) / 2;
    return glm::vec2(1.0f - static_cast<float>(plane), gear % 2 == 1 ? 1.0f : -1.0f);
}

void StartGearShift(VehicleGearShift& shift, int gear)
{
    if (gear == shift.to)
    {
        return;
    }
    // Once the lever has started for the old target it goes on from there (a jump mid-way through the
    // gate); the hand carries on from where it is, back towards the knob.
    const float moveEnd = kGearShiftReachSeconds + kGearShiftMoveSeconds;
    if (shift.seconds >= kGearShiftReachSeconds)
    {
        shift.from = shift.to;
    }
    shift.to = gear;
    if (shift.seconds >= kGearShiftSeconds)
    {
        shift.seconds = 0.0f;
    }
    else if (shift.seconds >= moveEnd)
    {
        const float handThere = 1.0f - (shift.seconds - moveEnd) / kGearShiftReturnSeconds;
        shift.seconds = handThere * kGearShiftReachSeconds;
    }
    else
    {
        shift.seconds = std::min(shift.seconds, kGearShiftReachSeconds);
    }
}

void AdvanceGearShift(VehicleGearShift& shift, float deltaSeconds)
{
    shift.seconds = std::min(shift.seconds + std::max(deltaSeconds, 0.0f), 1e3f);
}

glm::vec2 GearLeverGatePosition(const VehicleGearShift& shift)
{
    const glm::vec2 from = GearGatePosition(shift.from);
    const glm::vec2 to = GearGatePosition(shift.to);
    const float t = std::clamp((shift.seconds - kGearShiftReachSeconds) / kGearShiftMoveSeconds, 0.0f, 1.0f);
    // From the old gear straight back to the gate's middle line, across it, then into the new gear.
    const std::array<glm::vec2, 4> path{from, glm::vec2(from.x, 0.0f), glm::vec2(to.x, 0.0f), to};
    std::array<float, 3> lengths{};
    float total = 0.0f;
    for (size_t index = 0; index < lengths.size(); ++index)
    {
        lengths[index] = glm::distance(path[index], path[index + 1]);
        total += lengths[index];
    }
    if (total <= 0.0f)
    {
        return to;
    }
    // Eased in and out over the whole way.
    float along = (t * t * (3.0f - 2.0f * t)) * total;
    for (size_t index = 0; index < lengths.size(); ++index)
    {
        if (along <= lengths[index] || index + 1 == lengths.size())
        {
            const float share = lengths[index] > 0.0f ? std::min(along / lengths[index], 1.0f) : 1.0f;
            return glm::mix(path[index], path[index + 1], share);
        }
        along -= lengths[index];
    }
    return to;
}

float GearShiftHandWeight(const VehicleGearShift& shift)
{
    float weight = 0.0f;
    if (shift.seconds < kGearShiftReachSeconds)
    {
        weight = shift.seconds / kGearShiftReachSeconds;
    }
    else if (shift.seconds < kGearShiftReachSeconds + kGearShiftMoveSeconds)
    {
        weight = 1.0f;
    }
    else
    {
        weight = 1.0f - (shift.seconds - kGearShiftReachSeconds - kGearShiftMoveSeconds) / kGearShiftReturnSeconds;
    }
    weight = std::clamp(weight, 0.0f, 1.0f);
    return weight * weight * (3.0f - 2.0f * weight);
}

glm::mat4 GearLeverTransform(const ModelGearLever& lever, const glm::vec2& gate, const glm::quat& vehicleToModel)
{
    const glm::vec3 restKnob = lever.knob - lever.pivot;
    const float length = glm::length(restKnob);
    if (length < 1e-4f)
    {
        return glm::mat4(1.0f);
    }
    // The knob's offset in vehicle space (+X the car's left, +Z forward), into the model's.
    const glm::vec3 offset = vehicleToModel * glm::vec3(gate.x * kGateAcrossMetres, 0.0f, gate.y * kGateForwardMetres);
    const glm::vec3 knob = glm::normalize(restKnob + offset) * length;
    return glm::translate(glm::mat4(1.0f), lever.pivot) * glm::mat4_cast(TurnBetween(restKnob, knob)) *
           glm::translate(glm::mat4(1.0f), -lever.pivot);
}
}
