#pragma once

#include <filesystem>
#include <optional>

namespace me
{

// Path arithmetic for asset browser operations. Comparisons are on absolute,
// lexically normal paths, case-insensitive on Windows.
namespace AssetPaths
{
// True when `path` is `ancestor` itself or lies beneath it.
bool IsSameOrInside(const std::filesystem::path& path, const std::filesystem::path& ancestor);

// Where `path` lives after `oldPath` was renamed to `newPath`: `newPath` when
// it was the renamed file, the same relative spot under `newPath` when it was
// inside a renamed folder, std::nullopt when the rename did not touch it.
std::optional<std::filesystem::path> Rebase(
    const std::filesystem::path& path,
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath);

// The first free "<stem>_copy<ext>", "<stem>_copy2<ext>", ... next to `path`.
std::filesystem::path UniqueCopyPath(const std::filesystem::path& path);
}
}
