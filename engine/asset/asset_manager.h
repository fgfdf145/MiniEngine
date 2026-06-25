#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct AssetManagerResult
{
    struct PasteRequest
    {
        std::string sourcePath;
        std::string destinationDirectory;
    };

    std::optional<std::string> selectedModelPath;   // explicit "Load Model" action
    bool wantsImportModel = false;
    std::vector<std::string> deleteRequests;         // one or more paths to delete
    std::optional<std::string> draggedModelPath;
    std::optional<PasteRequest> pasteRequest;
};

class AssetManager
{
public:
    explicit AssetManager(std::filesystem::path assetsRoot);

    void Refresh();
    AssetManagerResult Draw();

    const std::filesystem::path& GetAssetsRoot() const { return m_root; }
    const std::filesystem::path& GetCurrentDirectory() const { return m_currentDir; }
    void NavigateTo(const std::filesystem::path& dir);

private:
    enum class AssetType { Dir, Model, Material, Scene, Texture, Other };

    struct Entry
    {
        std::filesystem::path path;
        std::string name;
        bool isDir = false;
        AssetType type = AssetType::Other;
    };

    void ScanCurrentDir();
    void DrawToolbar(AssetManagerResult& result);
    void DrawBreadcrumb();
    void DrawEntryList(AssetManagerResult& result);
    void DrawEntryRow(const Entry& entry, int index, AssetManagerResult& result);
    void DrawEntryContextMenu(const Entry& entry, AssetManagerResult& result);
    void DrawBatchContextMenu(AssetManagerResult& result);
    void DrawPreviewPanel(AssetManagerResult& result);

    static AssetType ClassifyPath(const std::filesystem::path& p);
    static const char* TypeTag(AssetType t);
    static void PushTypeColor(AssetType t);

    std::filesystem::path m_root;
    std::filesystem::path m_currentDir;
    std::vector<Entry> m_entries;
    std::unordered_set<int> m_selectedIndices;
    int m_anchorIdx = -1;         // anchor for shift-range, also the focused preview item
    std::string m_clipboard;
    bool m_needsScan = true;
};
