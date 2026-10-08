#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace me
{
class SvgIcon;

struct AssetManagerResult
{
    struct PasteRequest
    {
        std::string sourcePath;
        std::string destinationDirectory;
    };

    std::optional<std::string> selectedModelPath; // explicit "Load Model" action
    std::optional<std::string> openScenePath;     // a scene double-clicked: open it in the editor
    std::optional<std::string> previewAudioPath;  // a sound double-clicked: play it, or stop it playing
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
        Audio,
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

    // Tiles dragged onto a folder (a folder tile, "..", or a breadcrumb segment) move there.
    // Like a rename, a move that breaks path references is staged until the user confirms it.
    struct PendingMove
    {
        std::vector<std::string> sourcePaths;
        std::string destinationDirectory;
    };

    void ScanCurrentDir();
    void DrawToolbar(AssetManagerResult& result);
    void DrawBreadcrumb();
    void DrawEntryList(AssetManagerResult& result);
    void DrawEntryTile(const Entry& entry, int index, AssetManagerResult& result);
    void DrawEntryContextMenu(const Entry& entry, int index, AssetManagerResult& result);
    void DrawBatchContextMenu(AssetManagerResult& result);
    void DrawPreviewPanel(AssetManagerResult& result);
    void DrawPreviewDetails(AssetManagerResult& result);
    void DrawDeleteConfirmModal(AssetManagerResult& result);
    void BuildPendingDeleteWarnings();

    void BeginRename(int index);
    void CommitRename();
    void PerformRename(const PendingRename& rename);
    // Renames or moves one file or folder and keeps the registry, caches and material
    // sidecars following it. False (with the reason in `ec`) when the filesystem refused.
    bool MoveOnDisk(const std::filesystem::path& source, const std::filesystem::path& target, bool isDir,
                    std::error_code& ec);
    void DrawRenameConfirmModal();
    void CancelRename();
    void CreateNewFolder();

    void DrawEntryDragSource(const Entry& entry, int index, AssetManagerResult& result);
    // Makes the last item a drop target that moves the dragged tiles into `destination`.
    void DrawMoveDropTarget(const std::filesystem::path& destination);
    void RequestMove(const std::vector<std::string>& sourcePaths, const std::filesystem::path& destination);
    void PerformMove(const PendingMove& move);
    void DrawMoveConfirmModal();

    static AssetType ClassifyPath(const std::filesystem::path& p);
    static const char* TypeTag(AssetType t);
    static const char* ShortTag(AssetType t);
    static const char* TypeIcon(AssetType t);
    static const SvgIcon* TileIcon(const Entry& entry);
    static void PushTypeColor(AssetType t);
    static unsigned int TypeColorU32(AssetType t);

    std::filesystem::path m_root;
    std::filesystem::path m_currentDir;
    std::vector<Entry> m_entries;
    std::unordered_set<int> m_selectedIndices;
    int m_anchorIdx = -1; // anchor for shift-range, also the focused preview item

    // Layout that follows the window: tiles stretch to fill each row, and the preview
    // panel under the list is as tall as what it showed last frame (capped at half the window).
    float m_tileWidth = 0.0f;
    float m_previewContentHeight = 0.0f;

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

    // What the drag started in this browser carries: the dragged tile, or the whole selection
    // when the tile was part of it.
    std::vector<std::string> m_draggedPaths;
    std::optional<PendingMove> m_pendingMove;
    std::vector<std::string> m_pendingMoveWarnings;
    bool m_openMoveModal = false;
};
}
