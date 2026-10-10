#pragma once

#include <engine/audio/sound_bank.h>
#include <engine/audio/sound_event.h>
#include <engine/physics/physics_world.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace me
{

class AudioEngine;

// What the car's sounds are made from each frame (VehicleDriveService gathers it).
struct VehicleSoundInput
{
    VehicleTelemetry telemetry;
    std::vector<VehicleWheelState> wheels;
    PhysicsPose body;
    // The pedals as the driver holds them, 0 to 1.
    float throttle = 0.0f;
    float brake = 0.0f;
    // The engine's rev limit (VehicleSettings::maxRpm).
    float maxRpm = 7000.0f;
    // Heard from the driver's seat: the game's interior events (engine_int, gear_int...) instead of the
    // exterior ones.
    bool cockpit = false;
    glm::vec3 listener{0.0f};
    // Paused, or the window out of focus: everything falls silent but keeps its place.
    bool silent = false;
};

// What VehicleSoundInput turns into: the parameters Assetto Corsa's car events read, and the one-shot
// events to start this frame. The parameter names are the ones in Kunos' banks.
struct VehicleSoundFrame
{
    float rpms = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    float boost = 0.0f;
    float speedKmh = 0.0f;
    // The suspension's fastest travel (m/s at the wheel, as a share of 1 m/s): the bodywork's rattle.
    float suspensionTravelSpeed = 0.0f;
    // Seconds since the engine last hit its limiter (the limiter event's pop fades over its first 20 ms).
    float limiterDecay = 1.0f;
    // The tyres' slide, 0 (gripping) to 1 (well past their peak), which the skid events' volume follows.
    float skid = 0.0f;
    // Degrees between the car's nose and the way to the listener (FMOD's Event Cone Angle), and metres.
    float coneAngleDegrees = 0.0f;
    float distance = 0.0f;
    // Start this frame: a gear change (up or down) and a backfire on lifting off at high revs.
    bool gearChange = false;
    bool gearUp = false;
    bool backfire = false;
};

// Kept between frames by UpdateVehicleSoundFrame.
struct VehicleSoundState
{
    bool started = false;
    int gear = 0;
    float limiterDecay = 1.0f;
    // The throttle a moment ago and how long since it was last held down: a lift off within that time at
    // high revs backfires.
    float throttleHeldSeconds = 1.0e3f;
    float backfireCooldown = 0.0f;
    std::vector<float> wheelTravel;
};

VehicleSoundFrame UpdateVehicleSoundFrame(const VehicleSoundInput& input, VehicleSoundState& state, float deltaSeconds);

// A car's FMOD events (its .sounds.yaml, from ImportFmodBank) playing while it is driven.
class VehicleSounds final
{
  public:
    // Null, with the reason in error, when the bank cannot be read or has no engine.
    static std::unique_ptr<VehicleSounds> Load(AudioEngine& audio, const std::filesystem::path& bankPath, std::string& error);
    ~VehicleSounds();

    void Update(const VehicleSoundInput& input, float deltaSeconds);

    // The events loaded, by name (engine_ext...), for a log line.
    std::string Describe() const;

  private:
    struct Event
    {
        std::string name;
        std::unique_ptr<SoundEventInstance> instance;
    };
    VehicleSounds() = default;
    SoundEventInstance* Find(const std::string& name) const;
    // The interior or exterior one of a pair, as the view asks.
    SoundEventInstance* Pick(const std::string& base, bool cockpit) const;
    void SetCommon(SoundEventInstance& event, const VehicleSoundFrame& frame) const;

    SoundBank m_bank;
    std::vector<Event> m_events;
    VehicleSoundState m_state;
    bool m_cockpit = false;
    bool m_running = false;
};

}
