#include <engine/audio/audio_engine.h>
#include <engine/audio/fmod_bank.h>
#include <engine/audio/sound_bank.h>
#include <engine/audio/sound_event.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr uint32_t kSampleRate = 48000;
constexpr float kPi = 3.14159265358979f;

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

std::filesystem::path TestDirectory()
{
    static const std::filesystem::path directory = []
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_sound_bank_tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path / "sounds");
        return path;
    }();
    return directory;
}

void WriteLittleEndian(std::ofstream& file, uint32_t value, int bytes)
{
    for (int index = 0; index < bytes; ++index)
    {
        file.put(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

// A mono 16-bit sine at half amplitude, at the engine's rate.
void WriteSineWav(const std::filesystem::path& path, float seconds, float frequency)
{
    const uint32_t frames = static_cast<uint32_t>(seconds * kSampleRate);
    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    WriteLittleEndian(file, 36 + frames * 2, 4);
    file.write("WAVEfmt ", 8);
    WriteLittleEndian(file, 16, 4);
    WriteLittleEndian(file, 1, 2);
    WriteLittleEndian(file, 1, 2);
    WriteLittleEndian(file, kSampleRate, 4);
    WriteLittleEndian(file, kSampleRate * 2, 4);
    WriteLittleEndian(file, 2, 2);
    WriteLittleEndian(file, 16, 2);
    file.write("data", 4);
    WriteLittleEndian(file, frames * 2, 4);
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const float sample = 0.5f * std::sin(2.0f * kPi * frequency * static_cast<float>(frame) / kSampleRate);
        WriteLittleEndian(file, static_cast<uint16_t>(static_cast<int16_t>(sample * 32767.0f)), 2);
    }
    Require(file.good(), "the test WAV was written");
}

std::unique_ptr<AudioEngine> CreateSilentEngine()
{
    AudioEngineOptions options;
    options.output = AudioOutput::None;
    options.sampleRate = kSampleRate;
    options.channels = 2;
    std::string error;
    std::unique_ptr<AudioEngine> audio = AudioEngine::Create(options, error);
    Require(audio != nullptr, "a device-less engine starts: " + error);
    return audio;
}

float RenderRms(AudioEngine& audio, float seconds)
{
    const uint64_t frames = static_cast<uint64_t>(seconds * kSampleRate);
    std::vector<float> mix(frames * 2);
    audio.Render(mix.data(), frames);
    double sum = 0.0;
    for (const float sample : mix)
    {
        sum += sample * sample;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(mix.size())));
}

// An engine in two loops crossfaded on the revs (low 0-4000 fading out from 3000, high 3000-8000 fading
// in), each pitched by the revs; a bus turns everything down 6 dB at no throttle; a gear change
// one-shot on a state sheet; and a looping timeline event.
SoundBank TestBank()
{
    SoundEventDesc engine;
    engine.name = "engine";
    engine.parameters = {{"rpms", 0.0f, 8000.0f, 1000.0f, 0.0f}, {"throttle", 0.0f, 1.0f, 0.0f, 0.0f}, {"state", 0.0f, 1.0f, 0.0f, 0.0f}};
    SoundBus master;
    master.name = "master";
    master.volumeDb = -2.0f;
    SoundBus group;
    group.name = "group";
    group.parent = 0;
    group.automations.push_back({"throttle", SoundProperty::Volume, {{0.0f, -6.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, {}});
    engine.buses = {master, group};

    SoundInstrument low;
    low.clips = {{"sounds/low.wav", 0.0f}};
    low.sheetParameter = "rpms";
    low.start = 0.0f;
    low.length = 4000.0f;
    low.looping = true;
    low.autopitchParameter = "rpms";
    low.autopitchReference = 2000.0f;
    low.bus = 1;
    low.automations.push_back({"rpms", SoundProperty::Fade, {{3000.0f, 1.0f, 0.0f}, {4000.0f, 0.0f, 0.0f}}, {}});
    SoundInstrument high = low;
    high.clips = {{"sounds/high.wav", 0.0f}};
    high.start = 3000.0f;
    high.length = 5000.0f;
    high.autopitchReference = 6000.0f;
    high.automations = {{"rpms", SoundProperty::Fade, {{3000.0f, 0.0f, 0.0f}, {4000.0f, 1.0f, 0.0f}}, {}}};
    SoundInstrument gear;
    gear.clips = {{"sounds/blip.wav", 0.0f}, {"sounds/blip.wav", 0.0f}};
    gear.sheetParameter = "state";
    gear.start = 0.5f;
    gear.length = 0.5f;
    gear.bus = 0;
    gear.pitchSemitones = 12.0f;
    engine.instruments = {low, high, gear};

    SoundEventDesc wind;
    wind.name = "wind";
    wind.timelineLoops = true;
    SoundBus windMaster;
    windMaster.name = "master";
    wind.buses = {windMaster};
    SoundInstrument blowing;
    blowing.clips = {{"sounds/blip.wav", 0.0f}};
    blowing.bus = 0;
    wind.instruments = {blowing};

    SoundBank bank;
    bank.events = {engine, wind};
    return bank;
}

void WriteTestSounds()
{
    WriteSineWav(TestDirectory() / "sounds" / "low.wav", 1.0f, 200.0f);
    WriteSineWav(TestDirectory() / "sounds" / "high.wav", 1.0f, 600.0f);
    WriteSineWav(TestDirectory() / "sounds" / "blip.wav", 0.1f, 1000.0f);
}

void EvaluatesCurvesAndMappings()
{
    const std::vector<SoundCurvePoint> line{{0.0f, 0.0f, 0.0f}, {10.0f, 1.0f, 0.0f}};
    Require(EvaluateSoundCurve(line, -5.0f) == 0.0f && EvaluateSoundCurve(line, 20.0f) == 1.0f, "a curve holds its ends");
    Require(Near(EvaluateSoundCurve(line, 2.5f), 0.25f, 1e-6f), "a flat shape is a straight line");
    const std::vector<SoundCurvePoint> late{{0.0f, 0.0f, 0.5f}, {10.0f, 1.0f, 0.0f}};
    const std::vector<SoundCurvePoint> early{{0.0f, 0.0f, -0.5f}, {10.0f, 1.0f, 0.0f}};
    Require(EvaluateSoundCurve(late, 5.0f) < 0.5f && EvaluateSoundCurve(early, 5.0f) > 0.5f, "a positive shape holds back, a negative one moves early");
    Require(Near(EvaluateSoundCurve(late, 10.0f), 1.0f, 1e-6f), "shaped segments still reach their end");

    // FMOD's fader taper: curve values below -20 are squeezed down to -80 dB.
    const std::vector<std::pair<float, float>> taper{{-42.0f, -80.0f}, {-20.0f, -20.0f}, {10.0f, 10.0f}};
    Require(EvaluateSoundMapping(taper, -6.0f) == -6.0f, "the top of the taper passes values through");
    Require(Near(EvaluateSoundMapping(taper, -31.0f), -50.0f, 1e-4f), "and the bottom stretches them");
    Require(EvaluateSoundMapping(taper, -100.0f) == -80.0f, "it holds its ends");
    Require(EvaluateSoundMapping({}, 3.0f) == 3.0f, "no mapping keeps the value");
    Require(SoundBankPathForModel("a/b/car.gltf") == std::filesystem::path("a/b/car.sounds.yaml"), "a model's bank sits beside it");
}

void SavesAndLoads()
{
    const SoundBank bank = TestBank();
    const std::filesystem::path path = TestDirectory() / "test.sounds.yaml";
    std::string error;
    Require(SaveSoundBank(path, bank, error), "the bank saves: " + error);
    const std::optional<SoundBank> loaded = LoadSoundBank(path, error);
    Require(loaded.has_value(), "and loads: " + error);
    Require(loaded->events.size() == 2 && loaded->Find("wind") != nullptr && loaded->Find("wind")->timelineLoops, "with its events");
    const SoundEventDesc& engine = *loaded->Find("engine");
    Require(engine.parameters.size() == 3 && engine.parameters[0].name == "rpms" && engine.parameters[0].maximum == 8000.0f, "its parameters");
    Require(engine.buses.size() == 2 && engine.buses[1].parent == 0 && engine.buses[0].volumeDb == -2.0f, "its buses");
    Require(engine.buses[1].automations.size() == 1 && engine.buses[1].automations[0].curve.size() == 2, "their automations");
    const SoundInstrument& high = engine.instruments[1];
    Require(high.clips.size() == 1 && high.clips[0].file == "sounds/high.wav" && high.sheetParameter == "rpms" && high.start == 3000.0f && high.length == 5000.0f &&
                high.looping && high.autopitchParameter == "rpms" && high.autopitchReference == 6000.0f && high.bus == 1,
            "and its instruments");
    Require(high.automations.size() == 1 && high.automations[0].property == SoundProperty::Fade, "with theirs");
    Require(engine.instruments[2].clips.size() == 2 && engine.instruments[2].pitchSemitones == 12.0f, "a multi instrument keeps its playlist");

    std::ofstream(TestDirectory() / "broken.sounds.yaml") << "events:\n  - name: x\n    buses: []\n    instruments:\n      - clips: []\n        bus: 3\n";
    Require(!LoadSoundBank(TestDirectory() / "broken.sounds.yaml", error) && error.find("bus 3") != std::string::npos, "a bus out of range is refused: " + error);
}

void PlaysTheSheetAndItsCrossfades()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const SoundBank bank = TestBank();
    std::string error;
    std::unique_ptr<SoundEventInstance> engine = SoundEventInstance::Create(*audio, *bank.Find("engine"), TestDirectory(), false, error);
    Require(engine != nullptr, "the event loads its sounds: " + error);

    engine->SetParameter("throttle", 1.0f);
    engine->SetParameter("rpms", 1000.0f);
    engine->Start();
    Require(engine->IsPlaying(), "the event plays");
    SoundEventInstance::VoiceState low = engine->VoiceStateOf(0);
    SoundEventInstance::VoiceState high = engine->VoiceStateOf(1);
    Require(low.playing && !high.playing, "at 1000 rpm only the low loop plays");
    // -2 dB of master; the group's throttle curve is 0 dB at full throttle.
    Require(Near(low.gain, std::pow(10.0f, -2.0f / 20.0f), 1e-4f), "at the buses' volume: " + std::to_string(low.gain));
    Require(Near(low.pitch, 0.5f, 1e-4f), "and half its pitch at half its reference revs: " + std::to_string(low.pitch));
    Require(RenderRms(*audio, 0.1f) > 0.1f, "it is heard");

    engine->SetParameter("rpms", 3500.0f);
    engine->Update(0.016f);
    low = engine->VoiceStateOf(0);
    high = engine->VoiceStateOf(1);
    Require(low.playing && high.playing, "both play where they overlap");
    Require(Near(low.gain, high.gain, 1e-4f) && Near(low.gain, 0.5f * std::pow(10.0f, -2.0f / 20.0f), 1e-4f), "half way through the crossfade each is at half");
    Require(Near(high.pitch, 3500.0f / 6000.0f, 1e-4f), "the high loop follows its own reference");

    engine->SetParameter("throttle", 0.0f);
    engine->Update(0.016f);
    Require(Near(engine->InstrumentVolumeDb(0), -8.0f, 1e-4f), "off the throttle the group bus takes 6 dB more: " + std::to_string(engine->InstrumentVolumeDb(0)));

    engine->SetParameter("rpms", 6000.0f);
    engine->Update(0.016f);
    Require(!engine->VoiceStateOf(0).playing && engine->VoiceStateOf(1).playing, "past its region the low loop stops");
    engine->SetParameter("rpms", 9000.0f);
    engine->Update(0.016f);
    Require(engine->Parameter("rpms") == 8000.0f && engine->VoiceStateOf(1).playing, "parameters clamp to their range, and the top region takes the top in");

    engine->Stop();
    Require(!engine->IsPlaying() && !engine->VoiceStateOf(1).playing, "Stop silences the event");
    RenderRms(*audio, 0.01f);
    Require(RenderRms(*audio, 0.05f) < 1e-4f, "nothing is heard after it");
}

void TriggersOneShotsOnEntering()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const SoundBank bank = TestBank();
    std::string error;
    std::unique_ptr<SoundEventInstance> engine = SoundEventInstance::Create(*audio, *bank.Find("engine"), TestDirectory(), false, error);
    Require(engine != nullptr, error);
    engine->Start();
    Require(!engine->VoiceStateOf(2).playing, "the gear sound waits outside its region");
    engine->SetParameter("state", 1.0f);
    engine->Update(0.016f);
    Require(engine->VoiceStateOf(2).playing && Near(engine->VoiceStateOf(2).pitch, 2.0f, 1e-4f), "entering it plays the one-shot an octave up");
    RenderRms(*audio, 0.2f);
    engine->Update(0.2f);
    Require(!engine->VoiceStateOf(2).playing, "it plays once, though the parameter stays inside");
    engine->SetParameter("state", 0.0f);
    engine->Update(0.016f);
    engine->SetParameter("state", 1.0f);
    engine->Update(0.016f);
    Require(engine->VoiceStateOf(2).playing, "and again on entering again");

    // Starting with the parameter already inside plays it at once (AC starts its gear events so).
    engine->Stop();
    engine->Start();
    Require(engine->VoiceStateOf(2).playing, "an event started inside the region plays the one-shot");
}

void LoopsItsTimeline()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const SoundBank bank = TestBank();
    std::string error;
    std::unique_ptr<SoundEventInstance> wind = SoundEventInstance::Create(*audio, *bank.Find("wind"), TestDirectory(), false, error);
    Require(wind != nullptr, error);
    wind->Start();
    Require(wind->VoiceStateOf(0).playing, "a timeline sound at 0 s plays as the event starts");
    RenderRms(*audio, 0.3f);
    wind->Update(0.3f);
    Require(wind->VoiceStateOf(0).playing, "and loops on with the timeline's loop region");
    wind->SetVolume(0.5f);
    wind->Update(0.016f);
    Require(Near(wind->VoiceStateOf(0).gain, 0.5f, 1e-4f), "the event's volume scales it");
}

void SeeksSlowParameters()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    SoundBank bank = TestBank();
    bank.events[0].parameters[1].seekSpeed = 2.0f;
    std::string error;
    std::unique_ptr<SoundEventInstance> engine = SoundEventInstance::Create(*audio, bank.events[0], TestDirectory(), false, error);
    Require(engine != nullptr, error);
    engine->SetParameter("throttle", 1.0f);
    Require(engine->Parameter("throttle") == 0.0f, "a parameter with a seek speed does not jump");
    engine->Update(0.25f);
    Require(Near(engine->Parameter("throttle"), 0.5f, 1e-5f), "it moves at its speed");
    engine->Update(1.0f);
    Require(engine->Parameter("throttle") == 1.0f, "and stops at its target");
}

std::filesystem::path AcRoot()
{
    if (const char* root = std::getenv("MINIENGINE_AC_ROOT"))
    {
        return root;
    }
    return "C:/Program Files (x86)/Steam/steamapps/common/assettocorsa";
}

// Kunos' R34 bank, when the game is installed: the layout this reader was worked out from.
void ReadsTheR34Bank()
{
    const std::filesystem::path kn5 = AcRoot() / "content/cars/ks_nissan_skyline_r34/skyline_r34_vspec.kn5";
    const std::filesystem::path bankPath = FindAcCarSoundBank(kn5);
    if (bankPath.empty())
    {
        std::cout << "  (skipped: no Assetto Corsa R34 at " << AcRoot().string() << ")\n";
        return;
    }
    const std::filesystem::path guids = FindAcSoundGuids(kn5);
    Require(!guids.empty(), "the game's GUIDs.txt is found from the car's kn5");
    std::string error;
    const std::optional<FmodBank> bank = ReadFmodBank(bankPath, ReadFmodGuids(guids), error);
    Require(bank.has_value(), "the R34's bank reads: " + error);
    Require(bank->samples.size() == 44, "it has 44 samples: " + std::to_string(bank->samples.size()));
    for (const FmodSample& sample : bank->samples)
    {
        Require(sample.sampleRate == 44100 && (sample.channels == 1 || sample.channels == 2) && !sample.pcm.empty(), "each a 44.1 kHz PCM sample: " + sample.name);
    }
    Require(bank->bank.events.size() == 16, "and 16 events: " + std::to_string(bank->bank.events.size()));
    for (const char* name : {"engine_ext", "engine_int", "turbo", "gear_ext", "gear_int", "limiter", "backfire_ext", "skid_ext", "wind", "wheel"})
    {
        Require(bank->bank.Find(name) != nullptr, std::string("named from GUIDs.txt: ") + name);
    }

    const SoundEventDesc& engine = *bank->bank.Find("engine_ext");
    size_t onRevs = 0;
    for (const SoundInstrument& instrument : engine.instruments)
    {
        onRevs += instrument.sheetParameter == "rpms" ? 1 : 0;
    }
    Require(onRevs == 22, "the outside engine places 22 loops on the revs: " + std::to_string(onRevs));
    const SoundParameter* rpms = engine.FindParameter("rpms");
    Require(rpms != nullptr && rpms->maximum == 20000.0f, "over 0 to 20000 rpm");
    bool idle = false;
    for (const SoundInstrument& instrument : engine.instruments)
    {
        if (instrument.clips[0].file == "sounds/rb26_4_ex_idle.wav")
        {
            idle = true;
            Require(instrument.looping && instrument.start == 0.0f && instrument.length == 2000.0f, "the idle loop covers 0-2000 rpm");
            Require(instrument.autopitchParameter == "rpms" && Near(instrument.autopitchReference, 1359.0f, 1.0f), "pitched to its recording's 1359 rpm");
            Require(Near(instrument.volumeDb, -6.5f, 1e-4f), "at -6.5 dB");
            Require(instrument.automations.size() == 1 && instrument.automations[0].property == SoundProperty::Fade, "fading out on the revs");
            Require(instrument.bus >= 0 && !engine.buses[static_cast<size_t>(instrument.bus)].automations.empty(), "through a bus the throttle turns");
        }
    }
    Require(idle, "the idle loop is there");
    const SoundEventDesc& gear = *bank->bank.Find("gear_ext");
    Require(gear.instruments.size() == 2 && !gear.instruments[0].looping && gear.instruments[0].sheetParameter == "state", "gear changes are one-shots on 'state'");
    const SoundEventDesc& pops = *bank->bank.Find("backfire_ext");
    Require(pops.instruments.size() == 1 && pops.instruments[0].clips.size() == 2 && pops.instruments[0].sheetParameter.empty(), "backfires pick one of two pops on the timeline");
    Require(bank->bank.Find("skid_ext")->timelineLoops, "the skid's timeline loops");

    // The whole import, and the outside engine heard through a rev sweep without a gap.
    const std::filesystem::path out = TestDirectory() / "r34" / "r34.sounds.yaml";
    std::filesystem::create_directories(out.parent_path());
    Require(ImportFmodBank(bankPath, guids, out, error), "the import writes the bank: " + error);
    const std::optional<SoundBank> saved = LoadSoundBank(out, error);
    Require(saved.has_value() && saved->events.size() == 16, "which loads back: " + error);
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    std::unique_ptr<SoundEventInstance> sweep = SoundEventInstance::Create(*audio, *saved->Find("engine_ext"), out.parent_path(), false, error);
    Require(sweep != nullptr, "the outside engine loads its WAVs: " + error);
    sweep->SetParameter("throttle", 1.0f);
    sweep->SetParameter("rpms", 900.0f);
    sweep->Start();
    float quietest = 1.0f;
    for (float revs = 900.0f; revs <= 7800.0f; revs += 100.0f)
    {
        sweep->SetParameter("rpms", revs);
        sweep->Update(0.05f);
        const float rms = RenderRms(*audio, 0.05f);
        Require(std::isfinite(rms), "the mix stays finite");
        quietest = std::min(quietest, rms);
    }
    Require(quietest > 0.01f, "it is heard all the way up the revs on full throttle: quietest " + std::to_string(quietest));
}
}

int main()
{
    try
    {
        EvaluatesCurvesAndMappings();
        WriteTestSounds();
        SavesAndLoads();
        PlaysTheSheetAndItsCrossfades();
        TriggersOneShotsOnEntering();
        LoopsItsTimeline();
        SeeksSlowParameters();
        ReadsTheR34Bank();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "sound bank tests passed\n";
    return 0;
}
