#include "atomic_file.h"

#include <fstream>
#include <system_error>

namespace me
{

namespace AtomicFile
{
bool Write(const std::filesystem::path& path, std::string_view content, std::string* error)
{
    const auto fail = [error](std::string message)
    {
        if (error != nullptr)
        {
            *error = std::move(message);
        }
        return false;
    };

    // The temporary's name ends in ".tmp", so directory scans that match on a
    // file's real extension never mistake a stray one for the file itself.
    const std::filesystem::path tempPath = path.parent_path() / (path.filename().string() + ".tmp");
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return fail("could not open '" + tempPath.string() + "' for writing");
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out.good())
        {
            out.close();
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            return fail("could not write '" + tempPath.string() + "'");
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(tempPath, path, renameEc);
    if (renameEc)
    {
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        return fail("could not replace '" + path.string() + "': " + renameEc.message());
    }
    return true;
}
}
}
