#include <engine/asset/asset_registry.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Each case owns an isolated asset tree so a leftover sidecar from one case
// cannot decide another case's arbitration.
class ScopedAssetRoot
{
  public:
    explicit ScopedAssetRoot(const char* caseName)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_root = (ec ? std::filesystem::temp_directory_path() : base) /
                 "miniengine_asset_registry_tests" / caseName;
        std::filesystem::remove_all(m_root, ec);
        std::filesystem::create_directories(m_root, ec);
        AssetRegistry::Initialize(m_root);
    }

    ~ScopedAssetRoot()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    ScopedAssetRoot(const ScopedAssetRoot&) = delete;
    ScopedAssetRoot& operator=(const ScopedAssetRoot&) = delete;

    const std::filesystem::path& Root() const
    {
        return m_root;
    }

  private:
    std::filesystem::path m_root;
};

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

void MintAndPersist()
{
    ScopedAssetRoot scope("mint");
    const std::filesystem::path asset = scope.Root() / "tex.png";
    WriteFile(asset, "not really a png");

    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);
    Require(!uuid.empty(), "registrable asset did not receive a uuid");
    Require(Exists(AssetRegistry::SidecarPathFor(asset)), "sidecar was not written");
    Require(AssetRegistry::GetOrCreateUuid(asset) == uuid, "second call minted a different uuid");

    const std::optional<std::filesystem::path> resolved = AssetRegistry::ResolveUuid(uuid);
    Require(resolved.has_value(), "minted uuid did not resolve");
}

void BoundaryRejection()
{
    ScopedAssetRoot scope("boundary");
    const std::filesystem::path inside = scope.Root() / "inside.png";
    const std::filesystem::path wrongType = scope.Root() / "notes.txt";
    const std::filesystem::path outside = scope.Root().parent_path() / "outside.png";
    WriteFile(inside, "x");
    WriteFile(wrongType, "x");
    WriteFile(outside, "x");

    Require(AssetRegistry::GetOrCreateUuid(wrongType).empty(), "non-registrable extension got a uuid");
    Require(AssetRegistry::GetOrCreateUuid(outside).empty(), "asset outside the root got a uuid");
    Require(!AssetRegistry::GetOrCreateUuid(inside).empty(), "asset inside the root was rejected");

    Require(AssetRegistry::IsUnderAssetsRoot(inside), "inside path reported as outside");
    Require(!AssetRegistry::IsUnderAssetsRoot(outside), "outside path reported as inside");
    Require(!AssetRegistry::IsUnderAssetsRoot(scope.Root()), "the root itself reported as under itself");

    std::error_code ec;
    std::filesystem::remove(outside, ec);
}

// An asset copied together with its sidecar leaves two files claiming one
// uuid. Scan order must not decide the winner: the sidecar's recorded file
// name breaks the tie. Asserted in both directions.
void DuplicateArbitration(bool originalFirst)
{
    ScopedAssetRoot scope(originalFirst ? "duplicate_original_first" : "duplicate_copy_first");

    // "aaa" sorts before "zzz", so naming controls which file the recursive
    // scan reaches first.
    const std::string originalName = originalFirst ? "aaa_original.png" : "zzz_original.png";
    const std::string copyName = originalFirst ? "zzz_copy.png" : "aaa_copy.png";

    const std::filesystem::path original = scope.Root() / originalName;
    const std::filesystem::path copy = scope.Root() / copyName;
    WriteFile(original, "pixels");
    WriteFile(copy, "pixels");

    // Both sidecars carry the same uuid, but each records the file name it was
    // written for. Only the original's matches its own file name.
    const std::string sharedUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const std::string sidecar = "asset:\n  uuid: " + sharedUuid + "\n  file: " + originalName + "\n";
    WriteFile(AssetRegistry::SidecarPathFor(original), sidecar);
    WriteFile(AssetRegistry::SidecarPathFor(copy), sidecar);

    AssetRegistry::RescanAssetTree();

    const std::string originalUuid = AssetRegistry::GetOrCreateUuid(original);
    const std::string copyUuid = AssetRegistry::GetOrCreateUuid(copy);
    Require(!originalUuid.empty() && !copyUuid.empty(), "arbitration left an asset unregistered");
    Require(originalUuid != copyUuid, "duplicate uuids were not arbitrated");
    Require(originalUuid == sharedUuid, "the original did not keep the shared uuid");

    const std::optional<std::filesystem::path> owner = AssetRegistry::ResolveUuid(sharedUuid);
    Require(owner.has_value(), "the shared uuid resolves to nothing after arbitration");
    Require(owner->filename() == originalName, "the shared uuid resolved to the copy");
}

void OrphanSidecarPruning()
{
    ScopedAssetRoot scope("orphan");
    const std::filesystem::path asset = scope.Root() / "gone.png";
    WriteFile(asset, "x");
    AssetRegistry::GetOrCreateUuid(asset);

    const std::filesystem::path sidecar = AssetRegistry::SidecarPathFor(asset);
    Require(Exists(sidecar), "sidecar was not created before the orphan check");

    std::error_code ec;
    std::filesystem::remove(asset, ec);
    AssetRegistry::RescanAssetTree();

    Require(!Exists(sidecar), "orphaned sidecar survived a rescan");
}

// A sidecar whose asset is present must survive a rescan. The orphan rule
// keys on "the asset file is absent", and std::filesystem::exists also
// returns false when it could not tell — that must not count as absence.
void OrphanJudgmentRequiresCleanEvidence()
{
    ScopedAssetRoot scope("orphan_evidence");
    const std::filesystem::path present = scope.Root() / "present.png";
    WriteFile(present, "x");
    AssetRegistry::GetOrCreateUuid(present);
    const std::filesystem::path presentSidecar = AssetRegistry::SidecarPathFor(present);
    Require(Exists(presentSidecar), "sidecar was not created");

    AssetRegistry::RescanAssetTree();
    Require(Exists(presentSidecar), "a sidecar whose asset is present was deleted as an orphan");

    // The genuine orphan case still deletes.
    const std::filesystem::path gone = scope.Root() / "gone.png";
    WriteFile(gone, "x");
    AssetRegistry::GetOrCreateUuid(gone);
    const std::filesystem::path goneSidecar = AssetRegistry::SidecarPathFor(gone);

    std::error_code ec;
    std::filesystem::remove(gone, ec);
    AssetRegistry::RescanAssetTree();
    Require(!Exists(goneSidecar), "a genuinely orphaned sidecar survived");
}

// The sidecar write must be atomic: either the old content or the new one,
// never a truncated file, and never a stray temporary left behind.
void SidecarWriteIsAtomic()
{
    ScopedAssetRoot scope("atomic_write");
    const std::filesystem::path asset = scope.Root() / "tex.png";
    WriteFile(asset, "x");

    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);
    Require(!uuid.empty(), "asset did not receive a uuid");

    const std::filesystem::path sidecar = AssetRegistry::SidecarPathFor(asset);
    Require(Exists(sidecar), "sidecar was not written");

    // No temporary file may be left in the directory.
    std::error_code ec;
    for (std::filesystem::directory_iterator it(scope.Root(), ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        Require(
            name.find(".tmp") == std::string::npos,
            "an atomic-write temporary file was left behind");
    }

    // And the sidecar must round-trip: a rescan re-reads it and keeps the uuid.
    AssetRegistry::RescanAssetTree();
    Require(AssetRegistry::GetOrCreateUuid(asset) == uuid, "sidecar did not round-trip through a rescan");
}

void ReferenceResolution()
{
    ScopedAssetRoot scope("resolve");
    const std::filesystem::path asset = scope.Root() / "models" / "mesh.glb";
    WriteFile(asset, "x");
    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);

    // Tier 1: a known uuid beats a stale stored path.
    const ResolvedAssetReference byUuid =
        AssetRegistry::ResolveReference(uuid, (scope.Root() / "old" / "mesh.glb").string());
    Require(byUuid.resolved, "known uuid did not resolve");
    Require(byUuid.healed, "uuid resolution against a stale path did not report healed");
    Require(std::filesystem::path(byUuid.path).filename() == "mesh.glb", "uuid resolved to the wrong file");

    // Tier 2: an unknown uuid with a live stored path adopts and registers it.
    const std::filesystem::path adopted = scope.Root() / "models" / "adopted.png";
    WriteFile(adopted, "x");
    const ResolvedAssetReference byPath = AssetRegistry::ResolveReference("", adopted.string());
    Require(byPath.resolved, "live stored path was not adopted");
    Require(!byPath.uuid.empty(), "adopted path was not registered");

    // Tier 3: a unique filename match anywhere in the tree heals a dead path.
    const ResolvedAssetReference byName =
        AssetRegistry::ResolveReference("", (scope.Root() / "moved" / "mesh.glb").string());
    Require(byName.resolved, "unique filename match did not resolve");
    Require(byName.healed, "filename match did not report healed");
    Require(byName.uuid == uuid, "filename match resolved to the wrong asset");

    // Miss: nothing found, and the caller's data comes back untouched.
    const std::string missingPath = (scope.Root() / "nope" / "absent.glb").string();
    const ResolvedAssetReference miss = AssetRegistry::ResolveReference("no-such-uuid", missingPath);
    Require(!miss.resolved, "an unresolvable reference reported success");
    Require(miss.path == missingPath, "an unresolvable reference rewrote the stored path");
    Require(miss.uuid == "no-such-uuid", "an unresolvable reference rewrote the stored uuid");
}

void RenameKeepsIdentity()
{
    ScopedAssetRoot scope("rename");
    const std::filesystem::path before = scope.Root() / "before.png";
    WriteFile(before, "x");
    const std::string uuid = AssetRegistry::GetOrCreateUuid(before);

    const std::filesystem::path after = scope.Root() / "after.png";
    std::error_code ec;
    std::filesystem::rename(before, after, ec);
    Require(!ec, "test could not rename the asset");
    AssetRegistry::OnAssetRenamed(before, after);

    Require(AssetRegistry::GetOrCreateUuid(after) == uuid, "file rename did not preserve the uuid");
    Require(Exists(AssetRegistry::SidecarPathFor(after)), "sidecar did not follow the rename");
    Require(!Exists(AssetRegistry::SidecarPathFor(before)), "old sidecar was left behind");

    // Directory rename: every uuid beneath it survives.
    const std::filesystem::path nested = scope.Root() / "bundle" / "nested.png";
    WriteFile(nested, "x");
    const std::string nestedUuid = AssetRegistry::GetOrCreateUuid(nested);

    const std::filesystem::path renamedDir = scope.Root() / "bundle_renamed";
    std::filesystem::rename(scope.Root() / "bundle", renamedDir, ec);
    Require(!ec, "test could not rename the directory");
    AssetRegistry::OnAssetRenamed(scope.Root() / "bundle", renamedDir);

    Require(
        AssetRegistry::GetOrCreateUuid(renamedDir / "nested.png") == nestedUuid,
        "directory rename did not preserve a nested uuid");
}

void RemovalPrunes()
{
    ScopedAssetRoot scope("removal");
    const std::filesystem::path asset = scope.Root() / "doomed.png";
    WriteFile(asset, "x");
    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);
    const std::filesystem::path sidecar = AssetRegistry::SidecarPathFor(asset);

    std::error_code ec;
    std::filesystem::remove(asset, ec);
    AssetRegistry::OnAssetRemoved(asset);

    Require(!AssetRegistry::ResolveUuid(uuid).has_value(), "removed asset still resolves by uuid");
    Require(!Exists(sidecar), "sidecar outlived its asset");

    // Directory removal prunes the whole subtree.
    const std::filesystem::path nested = scope.Root() / "bundle" / "nested.png";
    WriteFile(nested, "x");
    const std::string nestedUuid = AssetRegistry::GetOrCreateUuid(nested);

    std::filesystem::remove_all(scope.Root() / "bundle", ec);
    AssetRegistry::OnAssetRemoved(scope.Root() / "bundle");

    Require(!AssetRegistry::ResolveUuid(nestedUuid).has_value(), "directory removal left a nested entry");
}
}

int main()
{
    try
    {
        MintAndPersist();
        BoundaryRejection();
        DuplicateArbitration(true);
        DuplicateArbitration(false);
        OrphanSidecarPruning();
        OrphanJudgmentRequiresCleanEvidence();
        SidecarWriteIsAtomic();
        ReferenceResolution();
        RenameKeepsIdentity();
        RemovalPrunes();

        std::cout << "asset registry tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset registry tests failed: " << error.what() << '\n';
        return 1;
    }
}
