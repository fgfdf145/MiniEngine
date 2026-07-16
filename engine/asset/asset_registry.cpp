#include "asset_registry.h"

#include <log/log.h>
#include <uuid/uuid.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
constexpr const char* kSidecarSuffix = ".miniengine_asset.yaml";

struct RegistryState
{
    std::mutex mutex;
    std::filesystem::path root;
    bool initialized = false;
    std::unordered_map<std::string, std::string> uuidToPath; // uuid -> display path (original case)
    std::unordered_map<std::string, std::string> pathToUuid; // normalized path key -> uuid
};

RegistryState& State()
{
    static RegistryState state;
    return state;
}

std::string ToLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// Absolute, lexically normal, generic separators; lowercased on Windows where
// the filesystem is case-insensitive.
std::string NormalizeKey(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(p, ec);
    if (ec)
    {
        absolute = p;
    }
    std::string key = absolute.lexically_normal().generic_string();
#ifdef _WIN32
    key = ToLowerCopy(key);
#endif
    return key;
}

std::string DisplayPath(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(p, ec);
    if (ec)
    {
        absolute = p;
    }
    return absolute.lexically_normal().string();
}

bool HasRegistrableExtension(const std::filesystem::path& p)
{
    const std::string ext = ToLowerCopy(p.extension().string());
    // Models and textures: the asset types other files reference today.
    return ext == ".gltf" || ext == ".glb" ||
           ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".dds";
}

bool IsUnderRootLocked(const std::string& normalizedKey)
{
    const std::string rootKey = NormalizeKey(State().root);
    return normalizedKey.size() > rootKey.size() + 1 &&
           normalizedKey.compare(0, rootKey.size(), rootKey) == 0 &&
           normalizedKey[rootKey.size()] == '/';
}

std::filesystem::path SidecarPathForInternal(const std::filesystem::path& assetPath)
{
    return assetPath.parent_path() / (assetPath.filename().string() + kSidecarSuffix);
}

struct SidecarData
{
    std::string uuid;
    std::string fileName; // the asset file name recorded when the sidecar was written
};

SidecarData ReadSidecar(const std::filesystem::path& sidecarPath)
{
    try
    {
        const YAML::Node root = YAML::LoadFile(sidecarPath.string());
        return SidecarData{
            root["asset"]["uuid"].as<std::string>(""),
            root["asset"]["file"].as<std::string>("")
        };
    }
    catch (...)
    {
        return {};
    }
}

bool WriteSidecar(const std::filesystem::path& sidecarPath, const std::string& uuid, const std::filesystem::path& assetPath)
{
    YAML::Node asset(YAML::NodeType::Map);
    asset["uuid"] = uuid;
    asset["file"] = assetPath.filename().string(); // informational only
    YAML::Node root(YAML::NodeType::Map);
    root["asset"] = asset;

    std::ofstream out(sidecarPath, std::ios::trunc);
    if (!out)
    {
        LOG_WARN("Could not write asset sidecar '{}'", sidecarPath.string());
        return false;
    }
    out << root;
    return out.good();
}

void EraseEntryLocked(const std::string& key)
{
    RegistryState& state = State();
    const auto it = state.pathToUuid.find(key);
    if (it == state.pathToUuid.end())
    {
        return;
    }
    state.uuidToPath.erase(it->second);
    state.pathToUuid.erase(it);
}

// Registers one asset file: adopts its sidecar uuid, resolving duplicates in
// favor of the already-registered owner, or mints a fresh uuid. Returns the
// uuid ("" when the sidecar could not be created and no uuid exists).
std::string RegisterFileLocked(const std::filesystem::path& file)
{
    RegistryState& state = State();
    const std::string key = NormalizeKey(file);

    if (const auto it = state.pathToUuid.find(key); it != state.pathToUuid.end())
    {
        return it->second;
    }

    const std::filesystem::path sidecarPath = SidecarPathForInternal(file);
    std::error_code ec;
    SidecarData sidecar;
    if (std::filesystem::exists(sidecarPath, ec))
    {
        sidecar = ReadSidecar(sidecarPath);
    }
    std::string uuid = sidecar.uuid;

    if (!uuid.empty())
    {
        const auto owner = state.uuidToPath.find(uuid);
        if (owner != state.uuidToPath.end() && NormalizeKey(owner->second) != key)
        {
            std::error_code ownerEc;
            if (std::filesystem::exists(owner->second, ownerEc))
            {
                // The asset was copied together with its sidecar and both files
                // claim one uuid. Scan order must not decide who keeps it, so
                // break the tie with the file name recorded inside each
                // sidecar: a mismatch marks the file that was copied.
                const std::filesystem::path ownerPath(owner->second);
                const SidecarData ownerSidecar = ReadSidecar(SidecarPathForInternal(ownerPath));
                const bool claimerLooksOriginal = sidecar.fileName == file.filename().string();
                const bool ownerLooksOriginal = ownerSidecar.fileName == ownerPath.filename().string();

                if (claimerLooksOriginal && !ownerLooksOriginal)
                {
                    // The already-registered file is the copy: it gets the
                    // fresh identity and this file keeps the uuid.
                    const std::string ownerDisplay = owner->second;
                    const std::string ownerNewUuid = Uuid::GenerateV4();
                    WriteSidecar(SidecarPathForInternal(ownerPath), ownerNewUuid, ownerPath);
                    state.uuidToPath.erase(uuid);
                    state.pathToUuid[NormalizeKey(ownerPath)] = ownerNewUuid;
                    state.uuidToPath[ownerNewUuid] = ownerDisplay;
                    LOG_WARN(
                        "Duplicate asset uuid {}: '{}' is the copy, assigned new uuid {}",
                        uuid, ownerDisplay, ownerNewUuid
                    );
                }
                else
                {
                    // References keep pointing at the registered original.
                    LOG_WARN(
                        "Duplicate asset uuid {} ('{}' vs '{}'); assigning a new uuid to the copy",
                        uuid, owner->second, file.string()
                    );
                    uuid.clear();
                }
            }
            else
            {
                // The registered owner vanished (moved outside the editor);
                // this file inherits the uuid.
                EraseEntryLocked(NormalizeKey(owner->second));
            }
        }
    }

    if (uuid.empty())
    {
        uuid = Uuid::GenerateV4();
        WriteSidecar(sidecarPath, uuid, file);
    }

    state.pathToUuid[key] = uuid;
    state.uuidToPath[uuid] = DisplayPath(file);
    return uuid;
}

void ScanLocked()
{
    RegistryState& state = State();
    state.uuidToPath.clear();
    state.pathToUuid.clear();

    std::error_code ec;
    if (!std::filesystem::exists(state.root, ec) || ec)
    {
        LOG_WARN("Asset registry root does not exist: '{}'", state.root.string());
        return;
    }

    std::vector<std::filesystem::path> orphanedSidecars;
    for (std::filesystem::recursive_directory_iterator
             it(state.root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end;
         it.increment(ec))
    {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc)
        {
            continue;
        }

        const std::filesystem::path& path = it->path();
        const std::string name = path.filename().string();
        if (name.ends_with(kSidecarSuffix))
        {
            // A sidecar whose asset file is gone is an orphan and gets removed.
            const std::string assetName = name.substr(0, name.size() - std::strlen(kSidecarSuffix));
            std::error_code existsEc;
            if (assetName.empty() || !std::filesystem::exists(path.parent_path() / assetName, existsEc))
            {
                orphanedSidecars.push_back(path);
            }
            continue;
        }

        if (HasRegistrableExtension(path))
        {
            RegisterFileLocked(path);
        }
    }

    for (const std::filesystem::path& orphan : orphanedSidecars)
    {
        std::error_code removeEc;
        std::filesystem::remove(orphan, removeEc);
        if (!removeEc)
        {
            LOG_INFO("Removed orphaned asset sidecar: {}", orphan.string());
        }
    }

    LOG_INFO("Asset registry: {} asset(s) registered under '{}'", state.pathToUuid.size(), state.root.string());
}

void EnsureInitializedLocked()
{
    RegistryState& state = State();
    if (state.initialized)
    {
        return;
    }
    state.root = std::filesystem::path(MINIENGINE_ASSET_DIR).lexically_normal();
    state.initialized = true;
    ScanLocked();
}
}

// ---------------------------------------------------------------------------

namespace AssetRegistry
{
void Initialize(const std::filesystem::path& assetsRoot)
{
    std::lock_guard lock(State().mutex);
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(assetsRoot, ec);
    State().root = (ec ? assetsRoot : absolute).lexically_normal();
    State().initialized = true;
    ScanLocked();
}

void RescanAssetTree()
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();
    ScanLocked();
}

std::string GetOrCreateUuid(const std::filesystem::path& assetPath)
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();

    if (!HasRegistrableExtension(assetPath) || !IsUnderRootLocked(NormalizeKey(assetPath)))
    {
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(assetPath, ec) || ec)
    {
        return {};
    }
    return RegisterFileLocked(assetPath);
}

std::optional<std::filesystem::path> ResolveUuid(const std::string& uuid)
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();

    const auto it = State().uuidToPath.find(uuid);
    if (it == State().uuidToPath.end())
    {
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::exists(it->second, ec) || ec)
    {
        // Prune the dead entry so a later adopter can claim the uuid.
        EraseEntryLocked(NormalizeKey(it->second));
        return std::nullopt;
    }
    return std::filesystem::path(it->second);
}

ResolvedAssetReference ResolveReference(const std::string& uuid, const std::string& storedPath)
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();
    RegistryState& state = State();

    ResolvedAssetReference result;
    result.path = storedPath;
    result.uuid = uuid;

    // 1) The uuid is authoritative: it survives renames and moves.
    if (!uuid.empty())
    {
        const auto it = state.uuidToPath.find(uuid);
        if (it != state.uuidToPath.end())
        {
            std::error_code ec;
            if (std::filesystem::exists(it->second, ec) && !ec)
            {
                result.path = it->second;
                result.resolved = true;
                result.healed = !storedPath.empty() && NormalizeKey(storedPath) != NormalizeKey(it->second);
                return result;
            }
            EraseEntryLocked(NormalizeKey(it->second));
        }
    }

    // 2) The stored path still exists: adopt it (registering it on the fly).
    if (!storedPath.empty())
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(storedPath, ec) && !ec)
        {
            result.resolved = true;
            const std::string key = NormalizeKey(storedPath);
            result.uuid = (HasRegistrableExtension(storedPath) && IsUnderRootLocked(key))
                ? RegisterFileLocked(storedPath)
                : std::string{};
            return result;
        }
    }

    // 3) Last resort: a unique filename match anywhere in the asset tree.
    if (!storedPath.empty())
    {
        const std::string fileName = ToLowerCopy(std::filesystem::path(storedPath).filename().string());
        std::string matchedUuid;
        size_t matchCount = 0;
        for (const auto& [key, candidateUuid] : state.pathToUuid)
        {
            const size_t slash = key.rfind('/');
            const std::string candidateName =
                ToLowerCopy(slash == std::string::npos ? key : key.substr(slash + 1));
            if (candidateName == fileName)
            {
                matchedUuid = candidateUuid;
                ++matchCount;
            }
        }
        if (matchCount == 1)
        {
            const auto it = state.uuidToPath.find(matchedUuid);
            if (it != state.uuidToPath.end())
            {
                LOG_WARN(
                    "Asset reference '{}' resolved by filename to '{}' (uuid {})",
                    storedPath, it->second, matchedUuid
                );
                result.path = it->second;
                result.uuid = matchedUuid;
                result.resolved = true;
                result.healed = true;
                return result;
            }
        }
    }

    return result; // unresolved: caller keeps the stored data and reports it
}

void OnAssetRenamed(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();

    std::error_code ec;
    if (std::filesystem::is_directory(newPath, ec))
    {
        // The uuid sidecars moved along with the directory contents; a rescan
        // rebuilds every path key while all uuids stay stable.
        ScanLocked();
        return;
    }

    const std::filesystem::path oldSidecar = SidecarPathForInternal(oldPath);
    const std::filesystem::path newSidecar = SidecarPathForInternal(newPath);
    std::error_code sidecarEc;
    if (std::filesystem::exists(oldSidecar, sidecarEc))
    {
        std::filesystem::rename(oldSidecar, newSidecar, sidecarEc);
        if (sidecarEc)
        {
            LOG_WARN(
                "Could not move asset sidecar '{}' -> '{}': {}",
                oldSidecar.string(), newSidecar.string(), sidecarEc.message()
            );
        }
    }

    EraseEntryLocked(NormalizeKey(oldPath));
    if (HasRegistrableExtension(newPath) && IsUnderRootLocked(NormalizeKey(newPath)))
    {
        const std::string uuid = RegisterFileLocked(newPath);
        if (!uuid.empty())
        {
            // Refresh the informational file name inside the sidecar.
            WriteSidecar(SidecarPathForInternal(newPath), uuid, newPath);
        }
    }
}

void OnAssetRemoved(const std::filesystem::path& path)
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();
    RegistryState& state = State();

    const std::string key = NormalizeKey(path);
    EraseEntryLocked(key);

    // Directory delete: prune every registered entry underneath it (their
    // sidecar files died with the directory).
    const std::string prefix = key + "/";
    for (auto it = state.pathToUuid.begin(); it != state.pathToUuid.end();)
    {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
        {
            state.uuidToPath.erase(it->second);
            it = state.pathToUuid.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // File delete: the uuid sidecar next to it is now an orphan.
    const std::filesystem::path sidecar = SidecarPathForInternal(path);
    std::error_code ec;
    if (std::filesystem::exists(sidecar, ec))
    {
        std::filesystem::remove(sidecar, ec);
    }
}

bool IsRegistrableAsset(const std::filesystem::path& path)
{
    return HasRegistrableExtension(path);
}

std::filesystem::path SidecarPathFor(const std::filesystem::path& assetPath)
{
    return SidecarPathForInternal(assetPath);
}
}
