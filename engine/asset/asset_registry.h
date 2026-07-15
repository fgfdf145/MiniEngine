#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Result of resolving a serialized asset reference (uuid + last-known path)
// against the on-disk registry.
struct ResolvedAssetReference
{
    std::string path;      // best-known current path (may equal the stored path)
    std::string uuid;      // uuid of the resolved asset ("" when unknown)
    bool healed = false;   // the returned path differs from the stored one
    bool resolved = false; // false when neither uuid nor path found the asset
};

// Maps asset files under the assets root to stable UUIDs, persisted in
// "<file>.miniengine_asset.yaml" sidecars next to each asset (the suffix is
// already hidden by the asset browser and skipped by scene scans). All
// functions are thread-safe; the registry lazily initializes against
// MINIENGINE_ASSET_DIR on first use.
namespace AssetRegistry
{
// Point the registry at a different assets root and rescan immediately.
void Initialize(const std::filesystem::path& assetsRoot);

// Walks the asset tree: registers new assets (creating sidecars), prunes
// entries whose files disappeared, deletes orphaned sidecars, and assigns
// fresh uuids to duplicates (e.g. an asset copied together with its sidecar).
void RescanAssetTree();

// Returns the asset's uuid, creating and persisting one if needed. Returns ""
// for files that do not exist, are outside the assets root, or are not a
// registrable asset type.
std::string GetOrCreateUuid(const std::filesystem::path& assetPath);

// Returns the current path for a uuid if that asset still exists on disk.
std::optional<std::filesystem::path> ResolveUuid(const std::string& uuid);

// Resolves a serialized reference. The uuid wins over the stored path; a
// still-valid stored path is adopted (and registered) when the uuid is
// unknown; as a last resort a unique filename match in the asset tree is used.
ResolvedAssetReference ResolveReference(const std::string& uuid, const std::string& storedPath);

// Keep the registry (and uuid sidecars) consistent with asset browser
// operations. Both accept files and directories; call them after the
// filesystem operation succeeded.
void OnAssetRenamed(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
void OnAssetRemoved(const std::filesystem::path& path);

bool IsRegistrableAsset(const std::filesystem::path& path);
std::filesystem::path SidecarPathFor(const std::filesystem::path& assetPath);
}
