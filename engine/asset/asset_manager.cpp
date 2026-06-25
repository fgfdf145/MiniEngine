#include "asset_manager.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace
{
std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool IsModelExt(const std::filesystem::path& p)
{
    const std::string ext = ToLower(p.extension().string());
    return ext == ".gltf" || ext == ".glb";
}

bool IsMaterialFile(const std::filesystem::path& p)
{
    return p.filename().string().ends_with(".material.yaml");
}

bool IsSceneFile(const std::filesystem::path& p)
{
    const std::string ext = ToLower(p.extension().string());
    if (ext != ".yaml" && ext != ".yml")
    {
        return false;
    }
    const std::string name = p.filename().string();
    return !name.ends_with(".material.yaml") && !name.ends_with(".miniengine_asset.yaml");
}

bool IsTextureExt(const std::filesystem::path& p)
{
    const std::string ext = ToLower(p.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".dds";
}

bool IsHiddenAsset(const std::filesystem::path& p)
{
    return p.filename().string().ends_with(".miniengine_asset.yaml");
}
}

// ---------------------------------------------------------------------------

AssetManager::AssetManager(std::filesystem::path assetsRoot)
    : m_root(std::move(assetsRoot))
    , m_currentDir(m_root)
{
}

void AssetManager::Refresh()
{
    m_needsScan = true;
}

void AssetManager::NavigateTo(const std::filesystem::path& dir)
{
    m_currentDir = dir;
    m_selectedIndices.clear();
    m_anchorIdx = -1;
    m_needsScan = true;
}

// ---------------------------------------------------------------------------
// Public entry point

AssetManagerResult AssetManager::Draw()
{
    AssetManagerResult result;

    if (m_needsScan)
    {
        ScanCurrentDir();
        m_needsScan = false;
    }

    DrawToolbar(result);
    ImGui::Separator();
    DrawBreadcrumb();
    ImGui::Separator();
    DrawEntryList(result);
    DrawPreviewPanel(result);

    return result;
}

// ---------------------------------------------------------------------------
// Directory scan

void AssetManager::ScanCurrentDir()
{
    m_entries.clear();
    m_selectedIndices.clear();
    m_anchorIdx = -1;

    std::error_code ec;
    if (!std::filesystem::exists(m_currentDir, ec) || !std::filesystem::is_directory(m_currentDir, ec))
    {
        m_currentDir = m_root;
        return;
    }

    // ".." entry when not at root
    if (m_currentDir != m_root)
    {
        Entry up{};
        up.path = m_currentDir.parent_path();
        up.name = "..";
        up.isDir = true;
        up.type = AssetType::Dir;
        m_entries.push_back(std::move(up));
    }

    std::vector<Entry> dirs;
    std::vector<Entry> files;

    for (const auto& item : std::filesystem::directory_iterator(m_currentDir, ec))
    {
        if (ec)
        {
            break;
        }

        Entry e{};
        e.path = item.path();
        e.name = item.path().filename().string();
        e.isDir = item.is_directory(ec);

        if (e.isDir)
        {
            e.type = AssetType::Dir;
            dirs.push_back(std::move(e));
        }
        else
        {
            if (IsHiddenAsset(e.path))
            {
                continue;
            }
            e.type = ClassifyPath(e.path);
            files.push_back(std::move(e));
        }
    }

    const auto byName = [](const Entry& a, const Entry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    for (auto& d : dirs)
    {
        m_entries.push_back(std::move(d));
    }
    for (auto& f : files)
    {
        m_entries.push_back(std::move(f));
    }
}

// ---------------------------------------------------------------------------
// Classification helpers

AssetManager::AssetType AssetManager::ClassifyPath(const std::filesystem::path& p)
{
    if (IsModelExt(p))    return AssetType::Model;
    if (IsMaterialFile(p)) return AssetType::Material;
    if (IsSceneFile(p))    return AssetType::Scene;
    if (IsTextureExt(p))   return AssetType::Texture;
    return AssetType::Other;
}

const char* AssetManager::TypeTag(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:      return "[DIR]";
    case AssetType::Model:    return "[MDL]";
    case AssetType::Material: return "[MAT]";
    case AssetType::Scene:    return "[SCN]";
    case AssetType::Texture:  return "[TEX]";
    default:                  return "[   ]";
    }
}

void AssetManager::PushTypeColor(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.80f, 0.30f, 1.0f)); break;
    case AssetType::Model:    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.75f, 1.00f, 1.0f)); break;
    case AssetType::Material: ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.55f, 1.00f, 1.0f)); break;
    case AssetType::Scene:    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 1.00f, 0.60f, 1.0f)); break;
    case AssetType::Texture:  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.90f, 0.85f, 1.0f)); break;
    default:                  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.60f, 1.0f)); break;
    }
}

// ---------------------------------------------------------------------------
// UI sections

void AssetManager::DrawToolbar(AssetManagerResult& result)
{
    if (ImGui::Button("Import Model"))
    {
        result.wantsImportModel = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("Refresh"))
    {
        m_needsScan = true;
    }

    ImGui::SameLine();

    // Navigate to root shortcut
    if (ImGui::Button("Assets Root"))
    {
        NavigateTo(m_root);
    }
}

void AssetManager::DrawBreadcrumb()
{
    // Collect path segments from root to current
    std::vector<std::filesystem::path> segments;
    std::filesystem::path cursor = m_currentDir;
    while (cursor != m_root.parent_path() && cursor != cursor.parent_path())
    {
        segments.push_back(cursor);
        if (cursor == m_root)
        {
            break;
        }
        cursor = cursor.parent_path();
    }
    std::reverse(segments.begin(), segments.end());

    for (size_t i = 0; i < segments.size(); ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }

        const std::string label = segments[i].filename().string().empty()
            ? "assets"
            : segments[i].filename().string();

        const bool isCurrent = (i + 1 == segments.size());
        if (isCurrent)
        {
            ImGui::TextUnformatted(label.c_str());
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.75f, 1.00f, 1.0f));
            const std::string btnId = label + "##bc" + std::to_string(i);
            if (ImGui::SmallButton(btnId.c_str()))
            {
                NavigateTo(segments[i]);
            }
            ImGui::PopStyleColor();
        }
    }
}

void AssetManager::DrawEntryList(AssetManagerResult& result)
{
    constexpr float kPreviewPanelHeight = 100.0f;
    const float listHeight = std::max(
        ImGui::GetContentRegionAvail().y - kPreviewPanelHeight - ImGui::GetStyle().ItemSpacing.y,
        60.0f
    );
    if (ImGui::BeginChild("##asset_list", ImVec2(0.0f, listHeight), false))
    {
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
        {
            DrawEntryRow(m_entries[static_cast<size_t>(i)], i, result);
        }
    }
    ImGui::EndChild();
}

void AssetManager::DrawEntryRow(const Entry& entry, int index, AssetManagerResult& result)
{
    const bool isSelected = m_selectedIndices.count(index) > 0;

    PushTypeColor(entry.type);
    ImGui::TextUnformatted(TypeTag(entry.type));
    ImGui::PopStyleColor();
    ImGui::SameLine();

    const std::string selId = entry.name + "##" + std::to_string(index);
    if (ImGui::Selectable(selId.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick))
    {
        // ".." always navigates, never participates in multi-select
        if (entry.name == "..")
        {
            NavigateTo(entry.path);
            return;
        }

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.isDir)
        {
            NavigateTo(entry.path);
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();

        if (io.KeyShift && m_anchorIdx >= 0)
        {
            // Range select: fill from anchor to current, optionally merging with existing
            if (!io.KeyCtrl)
            {
                m_selectedIndices.clear();
            }
            const int lo = std::min(m_anchorIdx, index);
            const int hi = std::max(m_anchorIdx, index);
            for (int i = lo; i <= hi; ++i)
            {
                m_selectedIndices.insert(i);
            }
            // anchor stays unchanged during shift-extend
        }
        else if (io.KeyCtrl)
        {
            // Toggle this item
            if (m_selectedIndices.count(index))
            {
                m_selectedIndices.erase(index);
            }
            else
            {
                m_selectedIndices.insert(index);
            }
            m_anchorIdx = index;
        }
        else
        {
            // Plain click: select only this item
            m_selectedIndices.clear();
            m_selectedIndices.insert(index);
            m_anchorIdx = index;
        }
    }

    // Drag source (only for model files, drag the specific entry)
    if (entry.type == AssetType::Model && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        const std::string pathStr = entry.path.string();
        ImGui::SetDragDropPayload("ASSET_MODEL_PATH", pathStr.c_str(), pathStr.size() + 1);
        ImGui::TextUnformatted(entry.name.c_str());
        result.draggedModelPath = pathStr;
        ImGui::EndDragDropSource();
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem(selId.c_str()))
    {
        // Right-clicking an unselected item switches selection to just that item
        if (!isSelected)
        {
            m_selectedIndices.clear();
            m_selectedIndices.insert(index);
            m_anchorIdx = index;
        }

        if (m_selectedIndices.size() > 1)
        {
            DrawBatchContextMenu(result);
        }
        else
        {
            DrawEntryContextMenu(entry, result);
        }
        ImGui::EndPopup();
    }
}

void AssetManager::DrawPreviewPanel(AssetManagerResult& result)
{
    ImGui::Separator();

    if (m_selectedIndices.empty())
    {
        ImGui::TextDisabled("No file selected");
        return;
    }

    // Multi-selection summary
    if (m_selectedIndices.size() > 1)
    {
        size_t modelCount = 0;
        size_t dirCount = 0;
        size_t otherCount = 0;
        for (const int idx : m_selectedIndices)
        {
            if (idx < 0 || idx >= static_cast<int>(m_entries.size()))
            {
                continue;
            }
            const Entry& e = m_entries[static_cast<size_t>(idx)];
            if (e.name == "..")
            {
                continue;
            }
            if (e.isDir)      { ++dirCount; }
            else if (e.type == AssetType::Model) { ++modelCount; }
            else              { ++otherCount; }
        }
        ImGui::Text("%zu items selected", m_selectedIndices.size());
        if (modelCount > 0) { ImGui::Text("  Models:  %zu", modelCount); }
        if (dirCount   > 0) { ImGui::Text("  Folders: %zu", dirCount); }
        if (otherCount > 0) { ImGui::Text("  Other:   %zu", otherCount); }
        ImGui::TextDisabled("Shift+click to extend range, Ctrl+click to toggle");
        return;
    }

    // Single selection: use anchor as the focused item
    const int focusIdx = (m_anchorIdx >= 0 && m_anchorIdx < static_cast<int>(m_entries.size()))
        ? m_anchorIdx
        : *m_selectedIndices.begin();

    const Entry& entry = m_entries[static_cast<size_t>(focusIdx)];

    PushTypeColor(entry.type);
    ImGui::TextUnformatted(entry.name.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TypeTag(entry.type));

    if (!entry.isDir)
    {
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(entry.path, ec);
        if (!ec)
        {
            if (bytes < 1024u)
                ImGui::Text("Size: %llu B", static_cast<unsigned long long>(bytes));
            else if (bytes < 1024u * 1024u)
                ImGui::Text("Size: %.1f KB", static_cast<double>(bytes) / 1024.0);
            else
                ImGui::Text("Size: %.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
    }

    ImGui::TextDisabled("%s", entry.path.string().c_str());

    if (entry.type == AssetType::Model)
    {
        if (ImGui::SmallButton("Load into Scene"))
        {
            result.selectedModelPath = entry.path.string();
        }
    }
}

void AssetManager::DrawBatchContextMenu(AssetManagerResult& result)
{
    // Gather valid selected entries, skipping ".."
    std::vector<const Entry*> selected;
    size_t modelCount = 0;
    for (const int idx : m_selectedIndices)
    {
        if (idx < 0 || idx >= static_cast<int>(m_entries.size()))
        {
            continue;
        }
        const Entry& e = m_entries[static_cast<size_t>(idx)];
        if (e.name == "..")
        {
            continue;
        }
        selected.push_back(&e);
        if (e.type == AssetType::Model)
        {
            ++modelCount;
        }
    }

    if (selected.empty())
    {
        return;
    }

    ImGui::TextDisabled("%zu items selected", selected.size());
    ImGui::Separator();

    if (modelCount > 0)
    {
        const std::string loadLabel = "Load " + std::to_string(modelCount) + " Model(s) into Scene";
        if (ImGui::MenuItem(loadLabel.c_str()))
        {
            // Load the first selected model (subsequent ones require async queue support)
            for (const Entry* e : selected)
            {
                if (e->type == AssetType::Model)
                {
                    result.selectedModelPath = e->path.string();
                    break;
                }
            }
        }
        ImGui::Separator();
    }

    if (ImGui::MenuItem("Copy Paths to Clipboard"))
    {
        std::string combined;
        for (const Entry* e : selected)
        {
            if (!combined.empty())
            {
                combined += '\n';
            }
            combined += e->path.string();
        }
        ImGui::SetClipboardText(combined.c_str());
    }

    ImGui::Separator();

    const std::string deleteLabel = "Delete " + std::to_string(selected.size()) + " Items";
    if (ImGui::MenuItem(deleteLabel.c_str()))
    {
        for (const Entry* e : selected)
        {
            result.deleteRequests.push_back(e->path.string());
        }
    }
}

void AssetManager::DrawEntryContextMenu(const Entry& entry, AssetManagerResult& result)
{
    if (entry.type == AssetType::Model)
    {
        if (ImGui::MenuItem("Load Model"))
        {
            result.selectedModelPath = entry.path.string();
        }
        ImGui::Separator();
    }

    if (ImGui::MenuItem("Copy Path"))
    {
        ImGui::SetClipboardText(entry.path.string().c_str());
    }

    if (entry.name != ".." && ImGui::MenuItem("Copy"))
    {
        m_clipboard = entry.path.string();
    }

    if (!m_clipboard.empty() && entry.name != "..")
    {
        if (ImGui::MenuItem("Paste Copy Here"))
        {
            result.pasteRequest = AssetManagerResult::PasteRequest{
                m_clipboard,
                m_currentDir.string()
            };
        }
    }

    if (entry.name != "..")
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            result.deleteRequests.push_back(entry.path.string());
        }
    }
}
