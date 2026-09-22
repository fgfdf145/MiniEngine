#include "model_import_target.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace me
{

namespace
{
// Matches AssetRegistry's sidecar naming; kept literal so this unit does not
// depend on the registry.
constexpr std::string_view kUuidSidecarSuffix = ".miniengine_asset.yaml";

bool IsUuidSidecar(const std::filesystem::path& path)
{
    return path.filename().string().ends_with(kUuidSidecarSuffix);
}

std::filesystem::path WithSuffix(const std::filesystem::path& folder, const std::string& suffix)
{
    return folder.parent_path() / (folder.filename().string() + suffix);
}

// Deletes every file under `folder` except uuid sidecars, then every directory
// that ends up empty.
void ClearKeepingSidecars(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code typeEc;
        if (it->is_directory(typeEc))
        {
            directories.push_back(it->path());
        }
        else if (!IsUuidSidecar(it->path()))
        {
            files.push_back(it->path());
        }
    }
    if (ec)
    {
        throw std::runtime_error("Failed to list '" + folder.string() + "': " + ec.message());
    }

    for (const std::filesystem::path& file : files)
    {
        std::error_code removeEc;
        std::filesystem::remove(file, removeEc);
        if (removeEc)
        {
            throw std::runtime_error("Failed to delete '" + file.string() + "': " + removeEc.message());
        }
    }

    // Deepest first: the iterator lists a directory before its children.
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
    {
        std::error_code emptyEc;
        if (std::filesystem::is_empty(*it, emptyEc) && !emptyEc)
        {
            std::filesystem::remove(*it, emptyEc);
        }
    }
}
}

namespace ModelImportTarget
{
std::filesystem::path DefaultFolder(const std::filesystem::path& source, const std::filesystem::path& destinationDirectory)
{
    return destinationDirectory.filename() == source.stem() ? destinationDirectory
                                                            : destinationDirectory / source.stem();
}

bool IsOccupied(const std::filesystem::path& folder)
{
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec) || ec)
    {
        return false;
    }
    // A plain file with the folder's name blocks it just as well.
    if (!std::filesystem::is_directory(folder, ec) || ec)
    {
        return true;
    }
    return !std::filesystem::is_empty(folder, ec) || ec;
}

std::filesystem::path NextFreeFolder(const std::filesystem::path& folder)
{
    for (int suffix = 1;; ++suffix)
    {
        std::filesystem::path candidate = WithSuffix(folder, "_" + std::to_string(suffix));
        if (!IsOccupied(candidate))
        {
            return candidate;
        }
    }
}

std::filesystem::path StagingFolderFor(const std::filesystem::path& target)
{
    for (int suffix = 0;; ++suffix)
    {
        const std::string tag = suffix == 0 ? ".importing" : ".importing" + std::to_string(suffix);
        std::filesystem::path candidate = WithSuffix(target, tag);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec)
        {
            return candidate;
        }
    }
}

void ReplaceContents(const std::filesystem::path& target, const std::filesystem::path& staging)
{
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec)
    {
        throw std::runtime_error("Failed to create '" + target.string() + "': " + ec.message());
    }

    ClearKeepingSidecars(target);

    std::vector<std::filesystem::path> stagedFiles;
    for (std::filesystem::recursive_directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code typeEc;
        if (it->is_regular_file(typeEc) && !typeEc)
        {
            stagedFiles.push_back(it->path());
        }
    }
    if (ec)
    {
        throw std::runtime_error("Failed to list '" + staging.string() + "': " + ec.message());
    }

    for (const std::filesystem::path& file : stagedFiles)
    {
        const std::filesystem::path destination = target / file.lexically_relative(staging);
        std::error_code moveEc;
        std::filesystem::create_directories(destination.parent_path(), moveEc);
        if (!moveEc)
        {
            std::filesystem::rename(file, destination, moveEc);
        }
        if (moveEc)
        {
            throw std::runtime_error(
                "Failed to move '" + file.string() + "' to '" + destination.string() + "': " + moveEc.message());
        }
    }

    std::filesystem::remove_all(staging, ec);
}
}
}
