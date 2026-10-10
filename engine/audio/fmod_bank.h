#pragma once

#include "sound_bank.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace me
{

// Assetto Corsa's car sounds are FMOD Studio banks (content/cars/<car>/sfx/<car>.bank): a RIFF "FEV "
// file holding the events' layout and an FSB5 sample bank. The layout is undocumented; what is read here
// was worked out from Kunos' banks (docs/design/2026-10-10-ac-car-sounds-design.md): parameter sheets,
// timelines, instrument volume/pitch/auto pitch, multi-instrument playlists, group and master buses,
// and the automation curves on all of them. Effects, modulators and snapshots are not read.

// A sample from the bank's FSB5, as interleaved 16-bit PCM.
struct FmodSample
{
    std::string name;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    std::vector<int16_t> pcm;
};

struct FmodBank
{
    // Events with their clips named "sounds/<sample name>.wav" (ImportFmodBank writes them there).
    SoundBank bank;
    std::vector<FmodSample> samples;
};

// Event names by GUID from Assetto Corsa's content/sfx/GUIDs.txt ("{guid} event:/cars/<car>/<event>"),
// keyed by the GUID's text in lower case without braces.
std::unordered_map<std::string, std::string> ReadFmodGuids(const std::filesystem::path& path);

// The bank's events and samples; nullopt with the reason in error when it cannot be read. Events are named
// by the last part of their path in `guidNames` (engine_ext, gear_int...), or by their GUID without it.
// Only PCM samples are read (Kunos' banks are PCM16); a bank of compressed samples is an error.
std::optional<FmodBank> ReadFmodBank(const std::filesystem::path& path, const std::unordered_map<std::string, std::string>& guidNames, std::string& error);

// Reads the bank and writes its samples as WAV files to `<directory>/sounds/` and its events to
// `bankPath` (a .sounds.yaml in `directory`). Existing files are replaced.
bool ImportFmodBank(const std::filesystem::path& fmodBankPath, const std::filesystem::path& guidsPath, const std::filesystem::path& bankPath, std::string& error);

// An Assetto Corsa car's bank and GUIDs.txt from its kn5's folder (content/cars/<car>/<car>.kn5): the
// bank under sfx/ and the GUIDs two folders up under sfx/. Empty when the car has no bank.
std::filesystem::path FindAcCarSoundBank(const std::filesystem::path& kn5Path);
std::filesystem::path FindAcSoundGuids(const std::filesystem::path& kn5Path);

}
