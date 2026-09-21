#include "asset_references.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace me
{

namespace
{
constexpr std::string_view kSidecarSuffix = ".miniengine_asset.yaml";

// A document over this size is skipped rather than read into memory.
constexpr std::uintmax_t kMaxScanFileBytes = 64ull * 1024 * 1024;

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool IsScannableDocument(const std::filesystem::path& path)
{
    const std::string extension = ToLowerCopy(path.extension().string());
    if (extension != ".gltf" && extension != ".yaml" && extension != ".yml")
    {
        return false;
    }
    // A uuid sidecar names its own asset by design; that is not a reference.
    return !path.filename().string().ends_with(kSidecarSuffix);
}

struct IndexedDocument
{
    std::filesystem::file_time_type lastWriteTime{};
    std::string content;
};

struct ReferenceIndex
{
    std::mutex mutex;
    std::unordered_map<std::string, IndexedDocument> documents; // normalized path -> content
};

ReferenceIndex& Index()
{
    static ReferenceIndex index;
    return index;
}

// Returns the document's content, from the index when its timestamp is
// unchanged, otherwise by reading it. Returns false when it could not be read,
// in which case the caller skips it.
//
// Caller must hold Index().mutex: this touches the index without locking.
bool ReadDocument(const std::filesystem::path& path, std::string& contentOut)
{
    const std::string key = path.lexically_normal().string();

    std::error_code timeEc;
    const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(path, timeEc);

    ReferenceIndex& index = Index();
    if (!timeEc)
    {
        const auto it = index.documents.find(key);
        if (it != index.documents.end() && it->second.lastWriteTime == lastWriteTime)
        {
            contentOut = it->second.content;
            return true;
        }
    }

    std::error_code sizeEc;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeEc);
    if (sizeEc || size > kMaxScanFileBytes)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    contentOut.assign(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    // A timestamp we could not read means we cannot index it: re-read next
    // time rather than trusting a stale entry.
    if (!timeEc)
    {
        index.documents[key] = IndexedDocument{lastWriteTime, contentOut};
    }
    return true;
}
}

std::vector<AssetReference> FindReferencesTo(
    const std::filesystem::path& root,
    const std::vector<std::string>& names,
    const std::unordered_set<std::string>& excludePaths,
    size_t maxResults)
{
    std::vector<AssetReference> found;
    if (names.empty() || maxResults == 0)
    {
        return found;
    }

    std::lock_guard lock(Index().mutex);

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end;
         it.increment(ec))
    {
        if (found.size() >= maxResults)
        {
            break;
        }

        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc)
        {
            continue;
        }
        const std::filesystem::path& path = it->path();
        if (!IsScannableDocument(path))
        {
            continue;
        }
        if (excludePaths.count(path.lexically_normal().string()) > 0)
        {
            continue;
        }

        std::string content;
        if (!ReadDocument(path, content))
        {
            continue;
        }

        for (const std::string& name : names)
        {
            if (content.find(name) == std::string::npos)
            {
                continue;
            }
            found.push_back(AssetReference{name, path.filename().string()});
            if (found.size() >= maxResults)
            {
                break;
            }
        }
    }

    return found;
}
}
