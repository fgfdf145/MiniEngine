#pragma once

#include <filesystem>

namespace me
{
// The files AudioEngine reads, by extension: .wav, .mp3, .flac and .ogg (Vorbis). Apart from the
// engine, so the asset browser can tell them without linking miniaudio.
bool IsAudioFilePath(const std::filesystem::path& path);
}
