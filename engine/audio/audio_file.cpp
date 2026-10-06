#include "audio_file.h"

#include <engine/core/text/ascii.h>

namespace me
{
bool IsAudioFilePath(const std::filesystem::path& path)
{
    const std::string extension = ToLowerAscii(path.extension().string());
    return extension == ".wav" || extension == ".mp3" || extension == ".flac" || extension == ".ogg";
}
}
