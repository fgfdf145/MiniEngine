#pragma once

#include <filesystem>
#include <optional>
#include <string>

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

// A file name split into the part a rename edits and the suffix it keeps:
// "tree.glb" -> {"tree", ".glb"}, "tree_0.material.yaml" -> {"tree_0",
// ".material.yaml"}. A folder keeps no suffix.
struct RenameableName
{
    std::string editable;
    std::string suffix;
};
RenameableName SplitRenameableName(const std::string& name, bool isDirectory);

// True when renaming `source` to `target` would replace a different file or
// folder. A case-only rename on a case-insensitive filesystem finds `source`
// itself at `target`, which is not a clash.
bool RenameWouldClobber(const std::filesystem::path& source, const std::filesystem::path& target);

// The first free "<name>_copy<suffix>", "<name>_copy2<suffix>", ... next to
// `path`, splitting the name like SplitRenameableName.
std::filesystem::path UniqueCopyPath(const std::filesystem::path& path);
}
}
