#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace me
{

struct AssetManagerResult
{
    struct PasteRequest
    {
        std::string sourcePath;
        std::string destinationDirectory;
    };

    std::optional<std::string> selectedModelPath; // explicit "Load Model" action
    std::vector<std::string> batchLoadModelPaths; // "Load N Models": each placed as a new entity
    bool wantsImportModel = false;
    std::vector<std::string> deleteRequests; // one or more paths to delete
    std::optional<std::string> draggedModelPath;
    std::optional<PasteRequest> pasteRequest;

    struct RenamedAsset
    {
        std::string oldPath;
        std::string newPath;
    };
    std::vector<RenamedAsset> renamedAssets; // renames completed on disk this frame
};

class AssetManager
{
  public:
    explicit AssetManager(std::filesystem::path assetsRoot);

    void Refresh();
    AssetManagerResult Draw();

    const std::filesystem::path& GetAssetsRoot() const
    {
        return m_root;
    }
    const std::filesystem::path& GetCurrentDirectory() const
    {
        return m_currentDir;
    }
    void NavigateTo(const std::filesystem::path& dir);

  private:
    enum class AssetType
    {
        Dir,
        Model,
        Material,
        Scene,
        Texture,
        Other
    };

    struct Entry
    {
        std::filesystem::path path;
        std::string name;
        bool isDir = false;
        AssetType type = AssetType::Other;
    };

    // A rename whose target is referenced by other documents is staged until
    // the user confirms it, mirroring the delete flow. Paths, not indices: the
    // entry list can be rescanned between staging and confirming.
    struct PendingRename
    {
        std::string sourcePath;
        std::string newName;
        bool isDir = false;
    };

    void ScanCurrentDir();
    void DrawToolbar(AssetManagerResult& result);
    void DrawBreadcrumb();
    void DrawEntryList(AssetManagerResult& result);
    void DrawEntryTile(const Entry& entry, int index, AssetManagerResult& result);
    void DrawEntryContextMenu(const Entry& entry, int index, AssetManagerResult& result);
    void DrawBatchContextMenu(AssetManagerResult& result);
    void DrawPreviewPanel(AssetManagerResult& result);
    void DrawDeleteConfirmModal(AssetManagerResult& result);
    void BuildPendingDeleteWarnings();

    void BeginRename(int index);
    void CommitRename();
    void PerformRename(const PendingRename& rename);
    void DrawRenameConfirmModal();
    void CancelRename();
    void CreateNewFolder();

    static AssetType ClassifyPath(const std::filesystem::path& p);
    static const char* TypeTag(AssetType t);
    static const char* ShortTag(AssetType t);
    static void PushTypeColor(AssetType t);
    static unsigned int TypeColorU32(AssetType t);

    std::filesystem::path m_root;
    std::filesystem::path m_currentDir;
    std::vector<Entry> m_entries;
    std::unordered_set<int> m_selectedIndices;
    int m_anchorIdx = -1; // anchor for shift-range, also the focused preview item

    // The focused entry's uuid, recomputed only when the focus moves or the
    // entry list is rebuilt. GetOrCreateUuid takes the global registry mutex
    // and hits the filesystem; a background import holds that same mutex
    // across a whole-tree rescan, so calling it per frame stalls the UI.
    int m_previewUuidIndex = -1;
    std::string m_previewUuid;
    std::string m_clipboard;
    bool m_needsScan = true;
    // Shown in red under the toolbar: a browser action that failed, or a missing assets folder.
    std::string m_statusError;

    // Inline rename (context menu "Rename" or F2). A newly created folder is
    // renamed immediately: its name is parked here until the next scan.
    int m_renamingIndex = -1;
    bool m_renameFocusPending = false;
    char m_renameBuffer[256] = {};
    std::string m_renameSuffix; // kept extension, e.g. ".glb": a rename edits only the name before it
    std::string m_pendingRenameName;

    // Delete requests are staged here until the user confirms them in a modal;
    // only confirmed paths are emitted as AssetManagerResult::deleteRequests.
    std::vector<std::string> m_pendingDeletePaths;
    std::vector<std::string> m_pendingDeleteWarnings; // "'x.png' is referenced by ..." lines
    bool m_pendingDeleteHasDir = false;
    bool m_openDeleteModal = false;

    std::optional<PendingRename> m_pendingRename;
    std::vector<AssetManagerResult::RenamedAsset> m_completedRenames; // drained into Draw()'s result
    std::vector<std::string> m_pendingRenameWarnings;
    bool m_openRenameModal = false;
};
}
