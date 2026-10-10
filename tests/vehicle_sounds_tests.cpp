#include <engine/audio/audio_engine.h>
#include <engine/audio/fmod_bank.h>
#include <engine/editor/services/vehicle_sounds.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

bool Near(float value, float expected, float tolerance)
{
    return std::abs(value - expected) <= tolerance;
}

VehicleSoundInput Cruising()
{
    VehicleSoundInput input;
    input.telemetry.engineRpm = 3000.0f;
    input.telemetry.gear = 3;
    input.telemetry.forwardSpeed = 20.0f;
    input.telemetry.turboBoost = 0.4f;
    input.throttle = 0.5f;
    input.maxRpm = 8000.0f;
    input.wheels.resize(4);
    for (VehicleWheelState& wheel : input.wheels)
    {
        wheel.inContact = true;
    }
    // The listener 10 m ahead of the car's nose (+Z).
    input.listener = glm::vec3(0.0f, 0.0f, 10.0f);
    return input;
}

void MapsTheCarToTheParameters()
{
    VehicleSoundState state;
    VehicleSoundInput input = Cruising();
    VehicleSoundFrame frame = UpdateVehicleSoundFrame(input, state, 0.016f);
    Require(frame.rpms == 3000.0f && frame.throttle == 0.5f && Near(frame.speedKmh, 72.0f, 1e-3f) && Near(frame.boost, 0.4f, 1e-6f), "revs, throttle, speed and boost");
    Require(!frame.gearChange && !frame.backfire && frame.skid == 0.0f, "nothing starts while cruising");
    Require(Near(frame.coneAngleDegrees, 0.0f, 1e-3f) && Near(frame.distance, 10.0f, 1e-4f), "a listener dead ahead is at 0 degrees");
    input.listener = glm::vec3(0.0f, 0.0f, -10.0f);
    Require(Near(UpdateVehicleSoundFrame(input, state, 0.016f).coneAngleDegrees, 180.0f, 1e-2f), "and behind at 180");

    input.throttle = -1.0f;
    Require(UpdateVehicleSoundFrame(input, state, 0.016f).throttle == 0.0f, "the throttle never goes below 0");
}

void StartsGearChanges()
{
    VehicleSoundState state;
    VehicleSoundInput input = Cruising();
    UpdateVehicleSoundFrame(input, state, 0.016f);
    input.telemetry.gear = 4;
    VehicleSoundFrame frame = UpdateVehicleSoundFrame(input, state, 0.016f);
    Require(frame.gearChange && frame.gearUp, "a change up starts the gear event, up");
    Require(!UpdateVehicleSoundFrame(input, state, 0.016f).gearChange, "once");
    input.telemetry.gear = 2;
    frame = UpdateVehicleSoundFrame(input, state, 0.016f);
    Require(frame.gearChange && !frame.gearUp, "a change down starts it, down");
}

void BackfiresOnALiftAtHighRevs()
{
    VehicleSoundState state;
    VehicleSoundInput input = Cruising();
    input.telemetry.engineRpm = 6500.0f;
    input.throttle = 1.0f;
    UpdateVehicleSoundFrame(input, state, 0.016f);
    input.throttle = 0.0f;
    Require(UpdateVehicleSoundFrame(input, state, 0.1f).backfire, "lifting off at 6500 rpm pops the exhaust");
    input.throttle = 1.0f;
    UpdateVehicleSoundFrame(input, state, 0.1f);
    input.throttle = 0.0f;
    Require(!UpdateVehicleSoundFrame(input, state, 0.1f).backfire, "not again straight away");

    VehicleSoundState low;
    input = Cruising();
    input.throttle = 1.0f;
    UpdateVehicleSoundFrame(input, low, 0.016f);
    input.throttle = 0.0f;
    Require(!UpdateVehicleSoundFrame(input, low, 0.1f).backfire, "nor at 3000 rpm");

    VehicleSoundState slow;
    input.telemetry.engineRpm = 6500.0f;
    input.throttle = 1.0f;
    UpdateVehicleSoundFrame(input, slow, 0.016f);
    input.throttle = 0.5f;
    UpdateVehicleSoundFrame(input, slow, 0.5f);
    input.throttle = 0.0f;
    Require(!UpdateVehicleSoundFrame(input, slow, 0.016f).backfire, "nor when the lift was slow");
}

void PopsOnTheLimiter()
{
    VehicleSoundState state;
    VehicleSoundInput input = Cruising();
    Require(UpdateVehicleSoundFrame(input, state, 0.016f).limiterDecay == 1.0f, "away from the limiter the pop has long died");
    input.telemetry.engineRpm = 7990.0f;
    input.throttle = 1.0f;
    Require(UpdateVehicleSoundFrame(input, state, 0.016f).limiterDecay == 0.0f, "hitting it restarts the pop");
    input.telemetry.engineRpm = 7600.0f;
    Require(Near(UpdateVehicleSoundFrame(input, state, 0.01f).limiterDecay, 0.01f, 1e-6f), "which then fades with time");
}

void SkidsWhenTheTyresSlide()
{
    VehicleSoundState state;
    VehicleSoundInput input = Cruising();
    input.wheels[0].slipAngleDegrees = 4.0f;
    Require(UpdateVehicleSoundFrame(input, state, 0.016f).skid == 0.0f, "below the slip where they start, the tyres are quiet");
    input.wheels[1].slipAngleDegrees = -8.5f;
    Require(Near(UpdateVehicleSoundFrame(input, state, 0.016f).skid, 0.5f, 1e-4f), "half way up the slide, half the skid");
    input.wheels[2].slipRatio = 1.0f;
    Require(UpdateVehicleSoundFrame(input, state, 0.016f).skid == 1.0f, "a locked wheel skids in full");
    input.wheels[2].inContact = false;
    input.wheels[1].inContact = false;
    Require(UpdateVehicleSoundFrame(input, state, 0.016f).skid == 0.0f, "a wheel in the air does not");
}

void FollowsTheSuspension()
{
    VehicleSoundState state;
    VehicleSoundInput input = Cruising();
    UpdateVehicleSoundFrame(input, state, 0.01f);
    input.wheels[3].travel = 0.004f;
    Require(Near(UpdateVehicleSoundFrame(input, state, 0.01f).suspensionTravelSpeed, 0.4f, 1e-4f), "4 mm in 10 ms is 0.4 m/s of travel");
}

// The R34's own bank playing through a short drive, when Assetto Corsa is installed.
void PlaysTheR34()
{
    const char* root = std::getenv("MINIENGINE_AC_ROOT");
    const std::filesystem::path ac = root != nullptr ? root : "C:/Program Files (x86)/Steam/steamapps/common/assettocorsa";
    const std::filesystem::path kn5 = ac / "content/cars/ks_nissan_skyline_r34/skyline_r34_vspec.kn5";
    const std::filesystem::path fmod = FindAcCarSoundBank(kn5);
    if (fmod.empty())
    {
        std::cout << "  (skipped: no Assetto Corsa R34)\n";
        return;
    }
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "miniengine_vehicle_sounds_tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::filesystem::path bankPath = SoundBankPathForModel(directory / "skyline_r34_vspec.gltf");
    std::string error;
    Require(ImportFmodBank(fmod, FindAcSoundGuids(kn5), bankPath, error), "the R34's sounds import: " + error);

    AudioEngineOptions options;
    options.output = AudioOutput::None;
    options.sampleRate = 48000;
    options.channels = 2;
    std::unique_ptr<AudioEngine> audio = AudioEngine::Create(options, error);
    Require(audio != nullptr, error);
    std::unique_ptr<VehicleSounds> sounds = VehicleSounds::Load(*audio, bankPath, error);
    Require(sounds != nullptr, "they load for the drive: " + error);
    Require(sounds->Describe().find("engine_ext") != std::string::npos && sounds->Describe().find("horn") == std::string::npos, "with the engine, not the horn: " + sounds->Describe());

    VehicleSoundInput input = Cruising();
    std::vector<float> mix(4800 * 2);
    const auto level = [&]
    {
        audio->Render(mix.data(), 4800);
        double sum = 0.0;
        for (const float sample : mix)
        {
            sum += sample * sample;
        }
        return static_cast<float>(std::sqrt(sum / mix.size()));
    };
    sounds->Update(input, 0.1f);
    const float outside = level();
    Require(outside > 0.01f && std::isfinite(outside), "the engine is heard from outside: " + std::to_string(outside));
    input.cockpit = true;
    input.telemetry.gear = 4;
    sounds->Update(input, 0.1f);
    const float inside = level();
    Require(inside > 0.01f && std::isfinite(inside), "and from the cockpit, through a gear change: " + std::to_string(inside));
    input.silent = true;
    sounds->Update(input, 0.1f);
    level();
    Require(level() < 1e-4f, "paused, it falls silent");
}
}

int main()
{
    try
    {
        MapsTheCarToTheParameters();
        StartsGearChanges();
        BackfiresOnALiftAtHighRevs();
        PopsOnTheLimiter();
        SkidsWhenTheTyresSlide();
        FollowsTheSuspension();
        PlaysTheR34();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "vehicle sounds tests passed\n";
    return 0;
}
