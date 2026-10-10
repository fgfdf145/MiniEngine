#include "vehicle_sounds.h"

#include <engine/audio/audio_engine.h>

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// Events played with the car the whole drive, and the pairs whose interior or exterior half the view picks.
constexpr const char* kLoops[] = {"turbo", "limiter", "wind", "wheel", "bodywork"};
constexpr const char* kEngine = "engine";
constexpr const char* kSkid = "skid";

// A lift off the throttle within this long of holding it past kBackfireThrottle, above kBackfireRpm,
// pops the exhaust (AC fires its backfire event on such lifts).
constexpr float kBackfireThrottle = 0.7f;
constexpr float kBackfireLift = 0.1f;
constexpr float kBackfireWindowSeconds = 0.35f;
constexpr float kBackfireRpm = 4500.0f;
constexpr float kBackfireCooldownSeconds = 0.6f;

// The tyres slide past these: the slip angle (degrees) and slip ratio at which the skid starts, and
// how much further it takes to reach full volume.
constexpr float kSkidAngleStart = 5.0f;
constexpr float kSkidAngleSpan = 7.0f;
constexpr float kSkidRatioStart = 0.12f;
constexpr float kSkidRatioSpan = 0.3f;

// The air the wind event expects with no car ahead to draft (its draft sound takes over below about 1).
constexpr float kAirPressure = 1.1f;
}

VehicleSoundFrame UpdateVehicleSoundFrame(const VehicleSoundInput& input, VehicleSoundState& state, float deltaSeconds)
{
    const float dt = std::max(deltaSeconds, 0.0f);
    const VehicleTelemetry& telemetry = input.telemetry;
    VehicleSoundFrame frame;
    frame.rpms = std::max(telemetry.engineRpm, 0.0f);
    frame.throttle = std::clamp(input.throttle, 0.0f, 1.0f);
    frame.brake = std::clamp(input.brake, 0.0f, 1.0f);
    frame.boost = std::clamp(telemetry.turboBoost, 0.0f, 1.0f);
    frame.speedKmh = std::abs(telemetry.forwardSpeed) * 3.6f;

    if (!state.started)
    {
        state.started = true;
        state.gear = telemetry.gear;
    }
    if (telemetry.gear != state.gear)
    {
        // Into neutral and out of it are changes too; up is towards the higher gears.
        frame.gearChange = true;
        frame.gearUp = telemetry.gear > state.gear;
        state.gear = telemetry.gear;
    }

    // Bouncing off the limiter restarts the pop each time.
    state.limiterDecay += dt;
    if (frame.rpms >= input.maxRpm - 30.0f && frame.throttle > 0.5f)
    {
        state.limiterDecay = 0.0f;
    }
    frame.limiterDecay = std::min(state.limiterDecay, 1.0f);

    state.backfireCooldown = std::max(state.backfireCooldown - dt, 0.0f);
    state.throttleHeldSeconds = frame.throttle >= kBackfireThrottle ? 0.0f : state.throttleHeldSeconds + dt;
    if (frame.throttle <= kBackfireLift && state.throttleHeldSeconds <= kBackfireWindowSeconds && frame.rpms >= kBackfireRpm && state.backfireCooldown <= 0.0f)
    {
        frame.backfire = true;
        state.backfireCooldown = kBackfireCooldownSeconds;
        state.throttleHeldSeconds = 1.0e3f;
    }

    state.wheelTravel.resize(input.wheels.size(), 0.0f);
    for (size_t index = 0; index < input.wheels.size(); ++index)
    {
        const VehicleWheelState& wheel = input.wheels[index];
        if (dt > 0.0f)
        {
            frame.suspensionTravelSpeed = std::max(frame.suspensionTravelSpeed, std::abs(wheel.travel - state.wheelTravel[index]) / dt);
        }
        state.wheelTravel[index] = wheel.travel;
        if (wheel.inContact && frame.speedKmh > 3.0f)
        {
            const float angle = std::clamp((std::abs(wheel.slipAngleDegrees) - kSkidAngleStart) / kSkidAngleSpan, 0.0f, 1.0f);
            const float ratio = std::clamp((std::abs(wheel.slipRatio) - kSkidRatioStart) / kSkidRatioSpan, 0.0f, 1.0f);
            frame.skid = std::max(frame.skid, std::max(angle, ratio));
        }
    }
    frame.suspensionTravelSpeed = std::min(frame.suspensionTravelSpeed, 1.0f);

    const glm::vec3 position = glm::vec3(input.body.position);
    const glm::vec3 toListener = input.listener - position;
    frame.distance = glm::length(toListener);
    if (frame.distance > 1e-3f)
    {
        const glm::vec3 nose = input.body.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
        frame.coneAngleDegrees = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(nose), toListener / frame.distance), -1.0f, 1.0f)));
    }
    return frame;
}

std::unique_ptr<VehicleSounds> VehicleSounds::Load(AudioEngine& audio, const std::filesystem::path& bankPath, std::string& error)
{
    std::optional<SoundBank> bank = LoadSoundBank(bankPath, error);
    if (!bank)
    {
        return nullptr;
    }
    std::unique_ptr<VehicleSounds> sounds(new VehicleSounds());
    sounds->m_bank = std::move(*bank);
    const std::filesystem::path directory = bankPath.parent_path();
    for (const SoundEventDesc& desc : sounds->m_bank.events)
    {
        // The horn, the doors and a missed gear have nothing to set them off yet.
        if (desc.name == "horn" || desc.name == "door" || desc.name == "gear_grind")
        {
            continue;
        }
        // The interior events are heard from inside, without a place; the rest come from the car.
        const bool interior = desc.name.size() > 4 && desc.name.ends_with("_int");
        std::unique_ptr<SoundEventInstance> instance = SoundEventInstance::Create(audio, desc, directory, !interior, error);
        if (!instance)
        {
            return nullptr;
        }
        sounds->m_events.push_back({desc.name, std::move(instance)});
    }
    if (sounds->Find("engine_ext") == nullptr && sounds->Find("engine_int") == nullptr)
    {
        error = "'" + bankPath.string() + "' has no engine_ext or engine_int event";
        return nullptr;
    }
    return sounds;
}

VehicleSounds::~VehicleSounds() = default;

SoundEventInstance* VehicleSounds::Find(const std::string& name) const
{
    for (const Event& event : m_events)
    {
        if (event.name == name)
        {
            return event.instance.get();
        }
    }
    return nullptr;
}

SoundEventInstance* VehicleSounds::Pick(const std::string& base, bool cockpit) const
{
    SoundEventInstance* wanted = Find(base + (cockpit ? "_int" : "_ext"));
    return wanted != nullptr ? wanted : Find(base + (cockpit ? "_ext" : "_int"));
}

std::string VehicleSounds::Describe() const
{
    std::string text;
    for (const Event& event : m_events)
    {
        text += (text.empty() ? "" : ", ") + event.name;
    }
    return text;
}

void VehicleSounds::SetCommon(SoundEventInstance& event, const VehicleSoundFrame& frame) const
{
    event.SetParameter("rpms", frame.rpms);
    event.SetParameter("throttle", frame.throttle);
    event.SetParameter("brake", frame.brake);
    event.SetParameter("boost", frame.boost);
    event.SetParameter("speed", frame.speedKmh);
    event.SetParameter("susp_travel_speed", frame.suspensionTravelSpeed);
    event.SetParameter("decay", frame.limiterDecay);
    event.SetParameter("Event Cone Angle", frame.coneAngleDegrees);
    event.SetParameter("Distance", frame.distance);
    // A whole, undamaged car: below these the flat tyre and the broken suspension sound.
    event.SetParameter("inflation", 1.0f);
    event.SetParameter("suspension_damage", 0.0f);
    event.SetParameter("air_pressure", kAirPressure);
}

void VehicleSounds::Update(const VehicleSoundInput& input, float deltaSeconds)
{
    const VehicleSoundFrame frame = UpdateVehicleSoundFrame(input, m_state, deltaSeconds);
    const glm::vec3 position = glm::vec3(input.body.position);
    for (const Event& event : m_events)
    {
        SetCommon(*event.instance, frame);
        event.instance->SetPosition(position);
    }

    // The engine and the loops that go with it run the whole drive; the view picks the engine heard.
    SoundEventInstance* engine = Pick(kEngine, input.cockpit);
    SoundEventInstance* other = Pick(kEngine, !input.cockpit);
    if (!m_running || input.cockpit != m_cockpit)
    {
        if (other != nullptr && other != engine)
        {
            other->Stop();
        }
        if (engine != nullptr)
        {
            engine->Start();
        }
        if (!m_running)
        {
            for (const char* name : kLoops)
            {
                if (SoundEventInstance* loop = Find(name))
                {
                    loop->Start();
                }
            }
        }
        m_running = true;
        m_cockpit = input.cockpit;
    }

    if (frame.gearChange)
    {
        if (SoundEventInstance* gear = Pick("gear", input.cockpit))
        {
            gear->SetParameter("state", frame.gearUp ? 1.0f : 0.0f);
            gear->Start();
        }
    }
    if (frame.backfire)
    {
        if (SoundEventInstance* backfire = Pick("backfire", input.cockpit))
        {
            backfire->Start();
        }
    }

    // The skid plays while the tyres slide, its volume following how far.
    SoundEventInstance* skid = Pick(kSkid, input.cockpit);
    if (SoundEventInstance* otherSkid = Pick(kSkid, !input.cockpit); otherSkid != nullptr && otherSkid != skid)
    {
        otherSkid->Stop();
    }
    if (skid != nullptr)
    {
        if (frame.skid > 0.0f && !skid->IsPlaying())
        {
            skid->Start();
        }
        else if (frame.skid <= 0.0f && skid->IsPlaying())
        {
            skid->Stop();
        }
        skid->SetVolume(frame.skid);
        skid->SetPitch(0.9f + 0.2f * frame.skid);
    }

    const float volume = input.silent ? 0.0f : 1.0f;
    for (const Event& event : m_events)
    {
        if (event.instance.get() != skid)
        {
            event.instance->SetVolume(volume);
        }
        else if (input.silent)
        {
            event.instance->SetVolume(0.0f);
        }
        event.instance->Update(deltaSeconds);
    }
}

}
