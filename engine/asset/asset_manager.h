#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Actions produced by one ImGui frame of AssetManager::Draw().
// The caller (editor backend) executes the operations.
struct AssetManagerResult
{
    std::optional<std::string> selectedModelPath;   // double-click on a model
    bool wantsImportModel = false;                  // user pressed "Import"
    std::optional<std::string> deleteRequest;       // path to delete
    std::optional<std::string> draggedModelPath;    // currently being dragged (for viewport preview)
};

class AssetManager
{
public:
    explicit AssetManager(std::filesystem::path assetsRoot);

    // Trigger a directory re-scan on the next Draw() call.
    void Refresh();

    // Draw the asset browser panel. Call this inside an ImGui window.
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

    static AssetType ClassifyPath(const std::filesystem::path& p);
    static const char* TypeTag(AssetType t);
    static void PushTypeColor(AssetType t);

    std::filesystem::path m_root;
    std::filesystem::path m_currentDir;
    std::vector<Entry> m_entries;
    int m_selectedIdx = -1;
    std::string m_clipboard;
    bool m_needsScan = true;
};
