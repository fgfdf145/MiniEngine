#include <engine/audio/audio_engine.h>

#include "test_fixture_paths.h"

#include <cmath>
#include <cstdint>
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

std::filesystem::path TestDirectory()
{
    static const std::filesystem::path directory = []
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_audio_tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
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

// A mono 16-bit PCM WAV of a sine at half amplitude, at the engine's rate so nothing resamples it.
std::filesystem::path WriteSineWav(const std::filesystem::path& path, float seconds, float frequency = 440.0f)
{
    const uint32_t frames = static_cast<uint32_t>(seconds * kSampleRate);
    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    WriteLittleEndian(file, 36 + frames * 2, 4);
    file.write("WAVEfmt ", 8);
    WriteLittleEndian(file, 16, 4);
    WriteLittleEndian(file, 1, 2); // PCM
    WriteLittleEndian(file, 1, 2); // mono
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
    return path;
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

// Root mean square of each channel over the next `seconds` of the mix.
struct ChannelLevels
{
    float left = 0.0f;
    float right = 0.0f;
    float Both() const
    {
        return std::sqrt(0.5f * (left * left + right * right));
    }
};

ChannelLevels RenderLevels(AudioEngine& audio, float seconds)
{
    const uint64_t frames = static_cast<uint64_t>(seconds * kSampleRate);
    std::vector<float> mix(frames * 2);
    // Fewer frames come back when nothing is playing; the rest is silence either way.
    audio.Render(mix.data(), frames);
    double left = 0.0;
    double right = 0.0;
    for (uint64_t frame = 0; frame < frames; ++frame)
    {
        left += mix[frame * 2] * mix[frame * 2];
        right += mix[frame * 2 + 1] * mix[frame * 2 + 1];
    }
    return {static_cast<float>(std::sqrt(left / frames)), static_cast<float>(std::sqrt(right / frames))};
}

bool Near(float value, float expected, float tolerance)
{
    return std::abs(value - expected) <= tolerance;
}

void ClassifiesAudioFiles()
{
    Require(IsAudioFilePath("a/engine.wav") && IsAudioFilePath("music.MP3") && IsAudioFilePath("x.flac") && IsAudioFilePath("x.Ogg"),
            "wav, mp3, flac and ogg are audio, whatever their case");
    Require(!IsAudioFilePath("car.kn5") && !IsAudioFilePath("sounds.bank") && !IsAudioFilePath("wav"),
            "other files are not");
}

void RejectsWhatItCannotRead()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    std::string error;
    Require(!audio->CreateSound(TestDirectory() / "missing.wav", {}, error), "a missing file makes no sound");
    Require(error.find("missing.wav") != std::string::npos, "the error names the file: " + error);

    const std::filesystem::path notAudio = TestDirectory() / "not_audio.wav";
    std::ofstream(notAudio) << "this is not a RIFF file";
    error.clear();
    Require(!audio->CreateSound(notAudio, {}, error), "a file that does not decode makes no sound");
    Require(!error.empty(), "and says why");
}

void PlaysADecodedSound()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const std::filesystem::path wav = WriteSineWav(TestDirectory() / "sine.wav", 0.5f);
    std::string error;
    const SoundId sound = audio->CreateSound(wav, {}, error);
    Require(static_cast<bool>(sound), "the WAV loads: " + error);
    Require(Near(audio->LengthSeconds(sound), 0.5f, 0.001f), "its length is the file's");
    Require(!audio->IsPlaying(sound), "a new sound waits to be played");
    Require(RenderLevels(*audio, 0.05f).Both() == 0.0f, "nothing plays before Play");

    Require(audio->Play(sound), "Play starts it");
    const ChannelLevels playing = RenderLevels(*audio, 0.1f);
    // A sine of amplitude 0.5 has an RMS of 0.354; a mono sound not spatialized reaches both
    // speakers whole.
    Require(Near(playing.left, 0.354f, 0.01f) && Near(playing.right, 0.354f, 0.01f),
            "the sine comes out at its own level on both channels: " + std::to_string(playing.left) + ", " + std::to_string(playing.right));
    // The cursor is where the file has been read to, which runs a chunk (a few hundred frames) ahead
    // of what was mixed.
    const float cursor = audio->CursorSeconds(sound);
    Require(cursor >= 0.1f && cursor < 0.11f, "the cursor follows the mix: " + std::to_string(cursor));

    audio->SetVolume(sound, 0.5f);
    RenderLevels(*audio, 0.01f);
    Require(Near(RenderLevels(*audio, 0.05f).Both(), 0.177f, 0.01f), "the sound's volume scales it");

    audio->SetMasterVolume(0.0f);
    Require(RenderLevels(*audio, 0.05f).Both() < 1e-4f, "the master volume silences everything");
    audio->SetMasterVolume(1.0f);

    audio->Stop(sound);
    Require(!audio->IsPlaying(sound) && audio->CursorSeconds(sound) == 0.0f, "Stop stops and rewinds");
    Require(RenderLevels(*audio, 0.05f).Both() == 0.0f, "a stopped sound is silent");

    audio->DestroySound(sound);
    Require(!audio->Play(sound), "a destroyed sound cannot be played");
}

void EndsOrLoops()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const std::filesystem::path wav = WriteSineWav(TestDirectory() / "short.wav", 0.2f);
    std::string error;
    const SoundId once = audio->CreateSound(wav, {}, error);
    SoundDesc loopingDesc;
    loopingDesc.looping = true;
    const SoundId looping = audio->CreateSound(wav, loopingDesc, error);
    Require(once && looping, "both load: " + error);

    audio->Play(once);
    RenderLevels(*audio, 0.3f);
    Require(!audio->IsPlaying(once), "a sound that is not looping stops at its end");
    Require(audio->Play(once) && audio->IsPlaying(once), "and plays again from the start");
    audio->Stop(once);

    audio->Play(looping);
    RenderLevels(*audio, 0.5f);
    Require(audio->IsPlaying(looping), "a looping sound keeps playing past its end");
    Require(RenderLevels(*audio, 0.05f).Both() > 0.3f, "and is heard there");
}

void SpatialSoundsFollowTheListener()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const std::filesystem::path wav = WriteSineWav(TestDirectory() / "spatial.wav", 2.0f);
    // The engine's camera convention: right-handed, Y up, looking down -Z by default, so +X is on
    // the listener's right.
    audio->SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    std::string error;
    SoundDesc desc;
    desc.spatial = true;
    desc.position = glm::vec3(4.0f, 0.0f, 0.0f);
    const SoundId sound = audio->CreateSound(wav, desc, error);
    Require(static_cast<bool>(sound), "the spatial sound loads: " + error);
    audio->Play(sound);
    RenderLevels(*audio, 0.05f); // past the gain smoothing
    const ChannelLevels right = RenderLevels(*audio, 0.1f);
    Require(right.right > 2.0f * right.left, "a source on the right is heard on the right: " + std::to_string(right.left) + ", " + std::to_string(right.right));

    // Turning around puts it on the left.
    audio->SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    RenderLevels(*audio, 0.05f);
    const ChannelLevels left = RenderLevels(*audio, 0.1f);
    Require(left.left > 2.0f * left.right, "after turning round it is on the left");

    // Inverse distance with a 1 m reference: gain 1/d, so four times as far is a quarter as loud.
    audio->SetPosition(sound, glm::vec3(0.0f, 0.0f, 2.0f));
    RenderLevels(*audio, 0.05f);
    const float near = RenderLevels(*audio, 0.1f).Both();
    audio->SetPosition(sound, glm::vec3(0.0f, 0.0f, 8.0f));
    RenderLevels(*audio, 0.05f);
    const float far = RenderLevels(*audio, 0.1f).Both();
    Require(Near(near / far, 4.0f, 0.2f), "the level falls with distance as 1/d: ratio " + std::to_string(near / far));
}

void OneShotsAndPreviewsFinishOnTheirOwn()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const std::filesystem::path wav = WriteSineWav(TestDirectory() / "blip.wav", 0.1f);
    std::string error;
    Require(audio->PlayOneShot(wav, 1.0f, error), "a one-shot plays: " + error);
    Require(RenderLevels(*audio, 0.05f).Both() > 0.3f, "and is heard");
    RenderLevels(*audio, 0.1f);
    audio->Update();
    Require(RenderLevels(*audio, 0.05f).Both() == 0.0f, "then it is over");

    Require(audio->StartPreview(wav, error), "the preview plays: " + error);
    Require(audio->PreviewPath() == wav, "the preview names its file");
    Require(RenderLevels(*audio, 0.05f).Both() > 0.3f, "the preview is heard");
    RenderLevels(*audio, 0.1f);
    audio->Update();
    Require(audio->PreviewPath().empty(), "a preview that reached its end is gone");

    Require(!audio->StartPreview(TestDirectory() / "missing.ogg", error) && audio->PreviewPath().empty(),
            "a preview of a missing file plays nothing");
}

// Ogg Vorbis (through stb_vorbis), FLAC and MP3, decoded whole and streamed. Lossy formats come close
// to the sine's level rather than exactly to it.
void DecodesCompressedFormats()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    for (const char* extension : {"ogg", "flac", "mp3"})
    {
        const std::filesystem::path file = std::filesystem::path(MINIENGINE_AUDIO_FIXTURES) / (std::string("sine_440hz.") + extension);
        for (const bool stream : {false, true})
        {
            const std::string label = std::string(extension) + (stream ? " (streamed)" : " (decoded)");
            SoundDesc desc;
            desc.stream = stream;
            std::string error;
            const SoundId sound = audio->CreateSound(file, desc, error);
            Require(static_cast<bool>(sound), label + " loads: " + error);
            // A streamed Ogg is read through stb_vorbis's push interface, which cannot know its length.
            const float length = audio->LengthSeconds(sound);
            const bool lengthUnknown = stream && std::string(extension) == "ogg";
            Require(lengthUnknown ? length == 0.0f : Near(length, 0.25f, 0.03f), label + " lasts a quarter second: " + std::to_string(length));
            audio->Play(sound);
            RenderLevels(*audio, 0.05f);
            const float level = RenderLevels(*audio, 0.1f).Both();
            Require(Near(level, 0.354f, 0.03f), label + " plays the sine: level " + std::to_string(level));
            audio->DestroySound(sound);
        }
    }
}

// Opens the real playback device when the machine has one; a machine without one (a build server)
// is not a failure, only reported.
void ReportsThePlaybackDevice()
{
    std::string error;
    const std::unique_ptr<AudioEngine> audio = AudioEngine::Create({}, error);
    if (!audio)
    {
        std::cout << "no playback device: " << error << '\n';
        return;
    }
    Require(audio->Output() == AudioOutput::Device && audio->SampleRate() > 0 && audio->Channels() > 0,
            "a device engine takes the device's format");
}

void OpensFilesWithNonAsciiNames()
{
    std::unique_ptr<AudioEngine> audio = CreateSilentEngine();
    const std::filesystem::path wav = WriteSineWav(TestDirectory() / std::filesystem::path(u8"引擎声_тест.wav"), 0.1f);
    std::string error;
    Require(static_cast<bool>(audio->CreateSound(wav, {}, error)), "a file named in Chinese and Cyrillic opens: " + error);
}
}

int main()
{
    try
    {
        ClassifiesAudioFiles();
        RejectsWhatItCannotRead();
        PlaysADecodedSound();
        EndsOrLoops();
        SpatialSoundsFollowTheListener();
        OneShotsAndPreviewsFinishOnTheirOwn();
        DecodesCompressedFormats();
        OpensFilesWithNonAsciiNames();
        ReportsThePlaybackDevice();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "audio engine tests passed\n";
    return 0;
}
