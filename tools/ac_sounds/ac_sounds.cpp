// Assetto Corsa car sounds from the command line:
//
//   miniengine_ac_sounds import <car folder | car.kn5 | car.bank> <model.gltf>
//       Reads the car's FMOD bank and writes its samples to <model dir>/sounds/*.wav and its events to
//       <model>.sounds.yaml, the bank the drive plays (a car imported before sounds were imported gets
//       them this way, without importing it again).
//   miniengine_ac_sounds dump <model.sounds.yaml | car.bank>
//       Lists the events, their parameters and instruments.
//   miniengine_ac_sounds sweep <model.sounds.yaml> <out.wav> [event] [seconds] [throttle]
//       Renders the engine (engine_ext by default) through a rev sweep from idle to 8000 rpm and back,
//       to listen to the crossfades.

#include <engine/audio/audio_engine.h>
#include <engine/audio/fmod_bank.h>
#include <engine/audio/sound_bank.h>
#include <engine/audio/sound_event.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace me;

namespace
{
int Usage()
{
    std::cerr << "usage:\n"
                 "  miniengine_ac_sounds import <car folder | car.kn5 | car.bank> <model.gltf>\n"
                 "  miniengine_ac_sounds dump <model.sounds.yaml | car.bank>\n"
                 "  miniengine_ac_sounds sweep <model.sounds.yaml> <out.wav> [event] [seconds] [throttle]\n";
    return 2;
}

// The bank and the game's GUIDs.txt from whatever names the car.
bool ResolveCar(const std::filesystem::path& source, std::filesystem::path& bank, std::filesystem::path& guids)
{
    std::error_code error;
    if (source.extension() == ".bank")
    {
        bank = source;
        // content/cars/<car>/sfx/<car>.bank -> content/sfx/GUIDs.txt
        guids = source.parent_path().parent_path().parent_path().parent_path() / "sfx" / "GUIDs.txt";
        if (!std::filesystem::is_regular_file(guids, error))
        {
            guids.clear();
        }
        return std::filesystem::is_regular_file(bank, error);
    }
    std::filesystem::path kn5 = source;
    if (std::filesystem::is_directory(source, error))
    {
        kn5 = source / (source.filename().string() + ".kn5");
    }
    bank = FindAcCarSoundBank(kn5);
    guids = FindAcSoundGuids(kn5);
    return !bank.empty();
}

int Import(const std::filesystem::path& source, const std::filesystem::path& model)
{
    std::filesystem::path bank;
    std::filesystem::path guids;
    if (!ResolveCar(source, bank, guids))
    {
        std::cerr << "no FMOD bank found for '" << source.string() << "'\n";
        return 1;
    }
    const std::filesystem::path out = SoundBankPathForModel(model);
    std::string error;
    if (!ImportFmodBank(bank, guids, out, error))
    {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "wrote " << out.string() << " from " << bank.string() << (guids.empty() ? " (no GUIDs.txt: events named by GUID)" : "") << "\n";
    return 0;
}

void Dump(const SoundBank& bank)
{
    for (const SoundEventDesc& event : bank.events)
    {
        std::cout << event.name << (event.timelineLoops ? " (looping timeline)" : "") << "\n  parameters:";
        for (const SoundParameter& parameter : event.parameters)
        {
            std::cout << " " << parameter.name << " [" << parameter.minimum << ", " << parameter.maximum << "]";
        }
        std::cout << "\n";
        for (const SoundInstrument& instrument : event.instruments)
        {
            std::cout << "  ";
            for (size_t clip = 0; clip < instrument.clips.size(); ++clip)
            {
                std::cout << (clip > 0 ? "|" : "") << std::filesystem::path(instrument.clips[clip].file).stem().string();
            }
            if (instrument.sheetParameter.empty())
            {
                std::cout << " timeline " << instrument.start << " s";
            }
            else
            {
                std::cout << " " << instrument.sheetParameter << " " << instrument.start << ".." << instrument.start + instrument.length;
            }
            std::cout << (instrument.looping ? " loop" : "") << " " << instrument.volumeDb << " dB";
            if (!instrument.autopitchParameter.empty())
            {
                std::cout << " autopitch " << instrument.autopitchParameter << "@" << instrument.autopitchReference;
            }
            std::cout << "\n";
        }
    }
}

int DumpCommand(const std::filesystem::path& source)
{
    std::string error;
    if (source.extension() == ".bank")
    {
        std::filesystem::path bank;
        std::filesystem::path guids;
        ResolveCar(source, bank, guids);
        const std::optional<FmodBank> fmod = ReadFmodBank(bank, ReadFmodGuids(guids), error);
        if (!fmod)
        {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << fmod->samples.size() << " samples\n";
        Dump(fmod->bank);
        return 0;
    }
    const std::optional<SoundBank> bank = LoadSoundBank(source, error);
    if (!bank)
    {
        std::cerr << error << "\n";
        return 1;
    }
    Dump(*bank);
    return 0;
}

void WriteWav(const std::filesystem::path& path, const std::vector<float>& mix, uint32_t rate, uint32_t channels)
{
    std::ofstream file(path, std::ios::binary);
    const auto put = [&](uint32_t value, int size)
    {
        for (int index = 0; index < size; ++index)
        {
            file.put(static_cast<char>((value >> (8 * index)) & 0xFF));
        }
    };
    const uint32_t bytes = static_cast<uint32_t>(mix.size() * 2);
    file.write("RIFF", 4);
    put(36 + bytes, 4);
    file.write("WAVEfmt ", 8);
    put(16, 4);
    put(1, 2);
    put(channels, 2);
    put(rate, 4);
    put(rate * channels * 2, 4);
    put(channels * 2, 2);
    put(16, 2);
    file.write("data", 4);
    put(bytes, 4);
    for (const float sample : mix)
    {
        put(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f)), 2);
    }
}

int Sweep(const std::filesystem::path& bankPath, const std::filesystem::path& out, const std::string& eventName, float seconds, float throttle)
{
    std::string error;
    const std::optional<SoundBank> bank = LoadSoundBank(bankPath, error);
    if (!bank)
    {
        std::cerr << error << "\n";
        return 1;
    }
    const SoundEventDesc* desc = bank->Find(eventName);
    if (desc == nullptr)
    {
        std::cerr << "no event '" << eventName << "'\n";
        return 1;
    }
    AudioEngineOptions options;
    options.output = AudioOutput::None;
    options.sampleRate = 48000;
    options.channels = 2;
    std::unique_ptr<AudioEngine> audio = AudioEngine::Create(options, error);
    if (!audio)
    {
        std::cerr << error << "\n";
        return 1;
    }
    std::unique_ptr<SoundEventInstance> event = SoundEventInstance::Create(*audio, *desc, bankPath.parent_path(), false, error);
    if (!event)
    {
        std::cerr << error << "\n";
        return 1;
    }
    constexpr float kIdle = 1000.0f;
    constexpr float kTop = 8000.0f;
    constexpr float kStep = 0.01f;
    event->SetParameter("throttle", throttle);
    event->SetParameter("rpms", kIdle);
    event->Start();
    std::vector<float> mix;
    std::vector<float> block(static_cast<size_t>(kStep * 48000.0f) * 2);
    for (float time = 0.0f; time < seconds; time += kStep)
    {
        // Up for the first half, back down for the second.
        const float phase = time / seconds;
        const float rise = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
        event->SetParameter("rpms", kIdle + (kTop - kIdle) * rise);
        event->Update(kStep);
        std::fill(block.begin(), block.end(), 0.0f);
        audio->Render(block.data(), block.size() / 2);
        mix.insert(mix.end(), block.begin(), block.end());
    }
    WriteWav(out, mix, 48000, 2);
    std::cout << "wrote " << out.string() << ": " << eventName << ", " << seconds << " s, throttle " << throttle << "\n";
    return 0;
}
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        return Usage();
    }
    const std::string command = argv[1];
    if (command == "import" && argc == 4)
    {
        return Import(argv[2], argv[3]);
    }
    if (command == "dump" && argc == 3)
    {
        return DumpCommand(argv[2]);
    }
    if (command == "sweep" && argc >= 4)
    {
        const std::string event = argc > 4 ? argv[4] : "engine_ext";
        const float seconds = argc > 5 ? std::stof(argv[5]) : 12.0f;
        const float throttle = argc > 6 ? std::stof(argv[6]) : 1.0f;
        return Sweep(argv[2], argv[3], event, seconds, throttle);
    }
    return Usage();
}
