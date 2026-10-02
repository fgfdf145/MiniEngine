#pragma once

#include <engine/scene/scene_components.h>
#include <engine/suspension/suspension_rigs.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace me
{
struct RendererSharedState;
struct LoadedModelData;

// What the live seven-post rig feeds its pads.
enum class VehicleRigWaveform
{
    Sine,  // the mode's pattern at `frequency`
    Sweep, // Rill's sine sweep from 0.5 to 20 Hz over 60 cycles, again and again
    Step,  // the pads up and down by `amplitude` every half period of `frequency`
    Road,  // a random road at `roadSpeed` (left and right tracks, the rears following the fronts)
    // The pads level, the three body loaders pushing the body at `frequency` in the mode (heave force,
    // pitch moment or roll moment) by `bodyLoad` g: the body moves on its springs as it does under
    // braking or cornering.
    BodyLoads,
};

struct VehicleRigExcitation
{
    suspension::RigMode mode = suspension::RigMode::Heave;
    VehicleRigWaveform waveform = VehicleRigWaveform::Sine;
    float frequency = 1.5f; // Hz
    float amplitude = 0.02f; // m at the pads
    float roadSpeed = 30.0f; // m/s
    float roadRoughness = 2e-6f; // Rill's phi0, m^3
    // The body loaders' push in g: the sprung weight times this (heave), or times it and the centre of
    // mass's height (the pitch moment of braking, the roll moment of cornering, at this g).
    float bodyLoad = 1.0f;
    // Simulated seconds per real second: below 1 the motion plays in slow motion.
    float playbackRate = 0.25f;
    // The drawn motion's scale over the simulated one (pads, body and wheels alike).
    float exaggeration = 3.0f;
    bool friction = false;
};

// What the Suspension Rigs window plots: the last seconds of the run, sampled at 200 Hz.
struct VehicleRigStatus
{
    bool active = false;
    std::string vehicleName;
    double time = 0.0;          // simulated seconds since the start
    double inputFrequency = 0.0; // Hz the pads move at now (0 for the step and the road)
    std::vector<double> sampleTime;
    std::array<std::vector<double>, 4> pad;      // m
    std::array<std::vector<double>, 4> tyreLoad; // N
    std::array<std::vector<double>, 4> travel;   // m, bump positive
    std::vector<double> heave;  // m
    std::vector<double> pitch;  // deg, nose up
    std::vector<double> roll;   // deg, right side down
    std::array<double, 4> staticTyreLoad{};
    std::string lastError;
};

// The car on the rig: the selected model, the scene's rig props (entities tagged "Wheel pad FL" and
// so on, and "Aero loader ..."), what they were before, and the simulation.
struct VehicleRigSession
{
    struct Prop
    {
        entt::entity entity = entt::null;
        TransformComponent start;
        int corner = -1;       // a wheel pad's corner, or -1 for an aero loader
        glm::vec3 rigPoint{0.0f}; // an aero loader's top in the rig frame (x forward, y left, z up)
    };

    entt::entity entity = entt::null;
    std::string name;
    TransformComponent startTransform;
    glm::mat4 startMatrix{1.0f};
    std::shared_ptr<const LoadedModelData> model;
    // The rig frame in the model's space: its origin (the sprung mass's centre at the wheel centres'
    // height) and its axes.
    glm::vec3 origin{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 left{-1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    std::vector<Prop> props;

    suspension::CarModel car;
    std::unique_ptr<suspension::SevenPostRig> rig;
    std::unique_ptr<suspension::RandomRoad> road;
    VehicleRigExcitation excitation;
    bool friction = false;
    double time = 0.0;
    double pendingSeconds = 0.0;
    std::array<double, 4> pads{};
    // The input's own clock: since the mode or waveform last changed, and the sine's phase.
    double inputStart = 0.0;
    double phase = 0.0;
    double nextSample = 0.0;
    VehicleRigStatus history;
};

struct VehicleRigState
{
    std::unique_ptr<VehicleRigSession> session;
    std::string lastError;
};

namespace VehicleRigService
{
// Puts the selected car's model on the live seven-post rig. Throws when the entity is not a model
// with a suspension linkage, hub masses and tyre rates in its data.
void Start(RendererSharedState& state, entt::entity entity, const VehicleRigExcitation& excitation);
// Puts the car and the rig's props back where they were.
void Stop(RendererSharedState& state);
// The window's settings, taken each frame (a change of mode or waveform restarts the input).
void SetExcitation(RendererSharedState& state, const VehicleRigExcitation& excitation);
// Per frame: advances the simulation at 1 kHz by the frame's time times the playback rate and moves
// the car, its wheels and the props. False when the rig is not running.
bool Tick(RendererSharedState& state, float deltaSeconds);
VehicleRigStatus GetStatus(const RendererSharedState& state);
// Runs `action` with the car and props where they were (to save the scene), then puts them back.
void RunWithRigAtStart(RendererSharedState& state, const std::function<void()>& action);
}
}
