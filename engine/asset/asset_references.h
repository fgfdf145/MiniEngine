#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace me
{

// One document mentioning one doomed file name.
struct AssetReference
{
    std::string referencedName; // the file name that was searched for
    std::string referencedBy;   // file name of the document mentioning it
};

// Scans .gltf/.yaml documents under `root` for mentions of any of `names`,
// skipping uuid sidecars and anything in `excludePaths` (each entry spelled as
// lexically_normal().string()).
//
// Substring matching on the file name: it can flag a same-named file in
// another folder, but a spurious warning is cheap next to a silently broken
// reference.
//
// Backed by a process-wide index keyed on each document's last_write_time, so
// repeat scans re-read only what changed. The first scan still walks the whole
// tree; it is the repeat case -- which is what a rename triggers -- that the
// index makes affordable.
std::vector<AssetReference> FindReferencesTo(
    const std::filesystem::path& root,
    const std::vector<std::string>& names,
    const std::unordered_set<std::string>& excludePaths,
    size_t maxResults = 6);
}
