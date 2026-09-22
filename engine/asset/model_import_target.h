#pragma once

#include <filesystem>

namespace me
{

// What an import does when its model folder already holds files.
enum class ImportConflictPolicy
{
    FailIfExists, // refuse: the caller asks the user first
    KeepBoth,     // import into the first free "<folder>_N" sibling
    Overwrite,    // replace the folder's files, keeping their uuid sidecars
};

// Filesystem decisions of a model import: where it lands and how an occupied
// destination is handled. Independent of the model format.
namespace ModelImportTarget
{
// The folder an import of `source` into `destinationDirectory` lands in:
// "<destination>/<stem>", or the destination itself when it already carries
// the model's name.
std::filesystem::path DefaultFolder(const std::filesystem::path& source, const std::filesystem::path& destinationDirectory);

// True when the folder exists and holds any entry.
bool IsOccupied(const std::filesystem::path& folder);

// The first "<folder>_N" sibling (N >= 1) that is not occupied.
std::filesystem::path NextFreeFolder(const std::filesystem::path& folder);

// A fresh sibling folder to import into before replacing `target`, so a
// failed import leaves the target untouched.
std::filesystem::path StagingFolderFor(const std::filesystem::path& target);

// Replaces everything in `target` with the contents of `staging`, then deletes
// `staging`. Uuid sidecars already in `target` are kept, so a re-imported file
// at the same relative path keeps its uuid and scene references to it hold;
// sidecars whose file is gone are left for the registry rescan to prune.
// Throws std::runtime_error on failure.
void ReplaceContents(const std::filesystem::path& target, const std::filesystem::path& staging);
}
}
