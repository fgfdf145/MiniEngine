#include "asset_paths.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>

namespace me
{

namespace
{
std::string Key(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
    {
        absolute = path;
    }
    std::string key = absolute.lexically_normal().generic_string();
    while (key.size() > 1 && key.back() == '/')
    {
        key.pop_back();
    }
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
#endif
    return key;
}

// Length of the ancestor prefix when `path` is `ancestor` or beneath it.
std::optional<size_t> AncestorPrefix(const std::filesystem::path& path, const std::filesystem::path& ancestor)
{
    const std::string pathKey = Key(path);
    const std::string ancestorKey = Key(ancestor);
    if (pathKey == ancestorKey)
    {
        return ancestorKey.size();
    }
    if (pathKey.size() > ancestorKey.size() &&
        pathKey.compare(0, ancestorKey.size(), ancestorKey) == 0 &&
        pathKey[ancestorKey.size()] == '/')
    {
        return ancestorKey.size();
    }
    return std::nullopt;
}
}

namespace AssetPaths
{
bool IsSameOrInside(const std::filesystem::path& path, const std::filesystem::path& ancestor)
{
    return AncestorPrefix(path, ancestor).has_value();
}

std::optional<std::filesystem::path> Rebase(
    const std::filesystem::path& path,
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath)
{
    if (path.empty() || !IsSameOrInside(path, oldPath))
    {
        return std::nullopt;
    }

    // The relative part is taken from the original path, keeping its case.
    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec).lexically_normal();
    const std::filesystem::path absoluteOld = std::filesystem::absolute(oldPath, ec).lexically_normal();
    const std::filesystem::path relative = absolutePath.lexically_relative(absoluteOld);
    if (relative.empty() || relative == ".")
    {
        return newPath;
    }
    return (newPath / relative).lexically_normal();
}

RenameableName SplitRenameableName(const std::string& name, bool isDirectory)
{
    if (isDirectory)
    {
        return RenameableName{name, {}};
    }

    // Suffixes that span two dots and carry meaning as a whole.
    for (const std::string_view compound : {std::string_view(".material.yaml"), std::string_view(".miniengine_asset.yaml")})
    {
        if (name.size() > compound.size() && name.ends_with(compound))
        {
            return RenameableName{name.substr(0, name.size() - compound.size()), std::string(compound)};
        }
    }

    // path::extension() already treats ".gitignore" as a stem without one.
    const std::string extension = std::filesystem::path(name).extension().string();
    return RenameableName{name.substr(0, name.size() - extension.size()), extension};
}

bool RenameWouldClobber(const std::filesystem::path& source, const std::filesystem::path& target)
{
    std::error_code ec;
    if (!std::filesystem::exists(target, ec) && !ec)
    {
        return false;
    }
    std::error_code sameEc;
    return !std::filesystem::equivalent(source, target, sameEc) || sameEc;
}

std::filesystem::path UniqueCopyPath(const std::filesystem::path& path)
{
    std::error_code dirEc;
    const bool isDirectory = std::filesystem::is_directory(path, dirEc) && !dirEc;
    const RenameableName name = SplitRenameableName(path.filename().string(), isDirectory);
    for (int index = 1;; ++index)
    {
        const std::string copyTag = index == 1 ? "_copy" : "_copy" + std::to_string(index);
        std::filesystem::path candidate = path.parent_path() / (name.editable + copyTag + name.suffix);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec)
        {
            return candidate;
        }
    }
}
}
}
