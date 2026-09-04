#include "asset_manager.h"

#include "asset_registry.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace me
{

namespace
{
std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   {
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

// A renamed model keeps its "<stem>_<index>.material.yaml" sidecars attached
// by renaming them to the new stem.
void RenameModelMaterialSidecars(const std::filesystem::path& oldModelPath, const std::filesystem::path& newModelPath)
{
    if (!IsModelExt(oldModelPath))
    {
        return;
    }

    const std::string oldPrefix = oldModelPath.stem().string() + "_";
    const std::string newPrefix = newModelPath.stem().string() + "_";
    constexpr std::string_view kSuffix = ".material.yaml";

    std::error_code iterEc;
    for (const auto& item : std::filesystem::directory_iterator(oldModelPath.parent_path(), iterEc))
    {
        if (iterEc)
        {
            break;
        }
        const std::string name = item.path().filename().string();
        if (!name.starts_with(oldPrefix) || !name.ends_with(kSuffix))
        {
            continue;
        }
        const std::string indexPart =
            name.substr(oldPrefix.size(), name.size() - oldPrefix.size() - kSuffix.size());
        const bool isMaterialIndex =
            !indexPart.empty() &&
            std::all_of(indexPart.begin(), indexPart.end(), [](unsigned char c)
                        {
                            return std::isdigit(c) != 0;
                        });
        if (!isMaterialIndex)
        {
            continue;
        }

        std::error_code renameEc;
        std::filesystem::rename(
            item.path(),
            item.path().parent_path() / (newPrefix + indexPart + std::string(kSuffix)),
            renameEc);
    }
}

// Square tile layout (Unreal-style content browser)
constexpr float kTileWidth = 96.0f;
constexpr float kTileIconHeight = 64.0f;
constexpr float kTileHeight = 100.0f; // icon area + ~2 lines of label
}

// ---------------------------------------------------------------------------

AssetManager::AssetManager(std::filesystem::path assetsRoot)
    : m_root(std::move(assetsRoot)), m_currentDir(m_root)
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
    m_renamingIndex = -1;
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

    // A freshly created folder starts in rename mode once it shows up in the scan.
    if (!m_pendingRenameName.empty())
    {
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
        {
            if (m_entries[static_cast<size_t>(i)].name == m_pendingRenameName)
            {
                BeginRename(i);
                break;
            }
        }
        m_pendingRenameName.clear();
    }

    DrawToolbar(result);
    ImGui::Separator();
    DrawBreadcrumb();
    ImGui::Separator();
    DrawEntryList(result);
    DrawPreviewPanel(result);
    DrawDeleteConfirmModal(result);

    return result;
}

// ---------------------------------------------------------------------------
// Directory scan

void AssetManager::ScanCurrentDir()
{
    m_entries.clear();
    m_selectedIndices.clear();
    m_anchorIdx = -1;
    m_renamingIndex = -1;

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

    const auto byName = [](const Entry& a, const Entry& b)
    {
        return a.name < b.name;
    };
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
    if (IsModelExt(p))
        return AssetType::Model;
    if (IsMaterialFile(p))
        return AssetType::Material;
    if (IsSceneFile(p))
        return AssetType::Scene;
    if (IsTextureExt(p))
        return AssetType::Texture;
    return AssetType::Other;
}

const char* AssetManager::TypeTag(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:
        return "[DIR]";
    case AssetType::Model:
        return "[MDL]";
    case AssetType::Material:
        return "[MAT]";
    case AssetType::Scene:
        return "[SCN]";
    case AssetType::Texture:
        return "[TEX]";
    default:
        return "[   ]";
    }
}

void AssetManager::PushTypeColor(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.80f, 0.30f, 1.0f));
        break;
    case AssetType::Model:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.75f, 1.00f, 1.0f));
        break;
    case AssetType::Material:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.55f, 1.00f, 1.0f));
        break;
    case AssetType::Scene:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 1.00f, 0.60f, 1.0f));
        break;
    case AssetType::Texture:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.90f, 0.85f, 1.0f));
        break;
    default:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.60f, 1.0f));
        break;
    }
}

const char* AssetManager::ShortTag(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:
        return "DIR";
    case AssetType::Model:
        return "MDL";
    case AssetType::Material:
        return "MAT";
    case AssetType::Scene:
        return "SCN";
    case AssetType::Texture:
        return "TEX";
    default:
        return "FILE";
    }
}

unsigned int AssetManager::TypeColorU32(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:
        return IM_COL32(255, 204, 77, 255);
    case AssetType::Model:
        return IM_COL32(115, 191, 255, 255);
    case AssetType::Material:
        return IM_COL32(204, 140, 255, 255);
    case AssetType::Scene:
        return IM_COL32(128, 255, 153, 255);
    case AssetType::Texture:
        return IM_COL32(102, 230, 217, 255);
    default:
        return IM_COL32(153, 153, 158, 255);
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
        // Also pick up files changed outside the editor (new/copied/moved assets).
        AssetRegistry::RescanAssetTree();
        m_needsScan = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("New Folder"))
    {
        CreateNewFolder();
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
        60.0f);
    if (ImGui::BeginChild("##asset_list", ImVec2(0.0f, listHeight), false))
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const int columns = std::max(
            1,
            static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (kTileWidth + style.ItemSpacing.x)));

        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
        {
            if (i % columns != 0)
            {
                ImGui::SameLine();
            }
            DrawEntryTile(m_entries[static_cast<size_t>(i)], i, result);
        }

        // F2 renames the single selected entry
        if (m_renamingIndex < 0 && m_selectedIndices.size() == 1 &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_F2, false))
        {
            BeginRename(*m_selectedIndices.begin());
        }

        // Right-click on empty space
        if (ImGui::BeginPopupContextWindow("##asset_list_ctx",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("New Folder"))
            {
                CreateNewFolder();
            }
            if (!m_clipboard.empty() && ImGui::MenuItem("Paste Copy Here"))
            {
                result.pasteRequest = AssetManagerResult::PasteRequest{
                    m_clipboard,
                    m_currentDir.string()};
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

void AssetManager::DrawEntryTile(const Entry& entry, int index, AssetManagerResult& result)
{
    const bool isSelected = m_selectedIndices.count(index) > 0;
    const bool isRenaming = (index == m_renamingIndex);

    ImGui::PushID(index);
    ImGui::BeginGroup();
    const ImVec2 tileMin = ImGui::GetCursorScreenPos();

    bool navigated = false;
    if (!isRenaming)
    {
        if (ImGui::Selectable("##tile", isSelected, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(kTileWidth, kTileHeight)))
        {
            // ".." always navigates, never participates in multi-select
            if (entry.name == "..")
            {
                NavigateTo(entry.path);
                navigated = true;
            }
            else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.isDir)
            {
                NavigateTo(entry.path);
                navigated = true;
            }
            else
            {
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
        }

        if (!navigated)
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                ImGui::SetTooltip("%s", entry.name.c_str());
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
            if (ImGui::BeginPopupContextItem("##tile_ctx"))
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
                    DrawEntryContextMenu(entry, index, result);
                }
                ImGui::EndPopup();
            }
        }
    }
    else
    {
        // Icon area stays; the label line becomes an inline rename field.
        ImGui::Dummy(ImVec2(kTileWidth, kTileIconHeight));
        if (m_renameFocusPending)
        {
            ImGui::SetKeyboardFocusHere();
            m_renameFocusPending = false;
        }
        ImGui::SetNextItemWidth(kTileWidth);
        const bool committed = ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (committed)
        {
            CommitRename();
        }
        else if (ImGui::IsItemDeactivated())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                CancelRename();
            }
            else
            {
                CommitRename(); // focus lost commits, like Unreal / Explorer
            }
        }
    }

    // --- tile decorations (drawn over the invisible selectable) ---
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 typeCol = TypeColorU32(entry.type);

    const ImVec2 iconMin(tileMin.x + 16.0f, tileMin.y + 8.0f);
    const ImVec2 iconMax(tileMin.x + kTileWidth - 16.0f, tileMin.y + kTileIconHeight - 6.0f);

    if (entry.isDir)
    {
        // Folder glyph: tab + body
        const float tabWidth = (iconMax.x - iconMin.x) * 0.45f;
        drawList->AddRectFilled(iconMin, ImVec2(iconMin.x + tabWidth, iconMin.y + 10.0f), typeCol, 3.0f);
        drawList->AddRectFilled(ImVec2(iconMin.x, iconMin.y + 6.0f), iconMax, typeCol, 4.0f);
        drawList->AddRectFilled(ImVec2(iconMin.x, iconMin.y + 6.0f), ImVec2(iconMax.x, iconMin.y + 14.0f),
                                IM_COL32(255, 255, 255, 40), 4.0f);
    }
    else
    {
        drawList->AddRectFilled(iconMin, iconMax, IM_COL32(52, 54, 60, 255), 4.0f);
        drawList->AddRect(iconMin, iconMax, typeCol, 4.0f, 0, 2.0f);
        const char* tag = ShortTag(entry.type);
        const ImVec2 tagSize = ImGui::CalcTextSize(tag);
        drawList->AddText(ImVec2((iconMin.x + iconMax.x - tagSize.x) * 0.5f,
                                 (iconMin.y + iconMax.y - tagSize.y) * 0.5f),
                          typeCol, tag);
    }

    if (!isRenaming)
    {
        // Name label: wrapped to the tile width, clipped to two lines,
        // centered when it fits on one line
        const float labelTop = tileMin.y + kTileIconHeight;
        const float wrapWidth = kTileWidth - 6.0f;
        const ImVec2 textSize = ImGui::CalcTextSize(entry.name.c_str(), nullptr, false, wrapWidth);
        const float textX = (textSize.x < wrapWidth)
                                ? tileMin.x + (kTileWidth - textSize.x) * 0.5f
                                : tileMin.x + 3.0f;
        const ImVec4 clipRect(tileMin.x, labelTop, tileMin.x + kTileWidth, tileMin.y + kTileHeight);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, labelTop),
                          ImGui::GetColorU32(ImGuiCol_Text), entry.name.c_str(), nullptr,
                          wrapWidth, &clipRect);
    }

    ImGui::EndGroup();
    ImGui::PopID();
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
            if (e.isDir)
            {
                ++dirCount;
            }
            else if (e.type == AssetType::Model)
            {
                ++modelCount;
            }
            else
            {
                ++otherCount;
            }
        }
        ImGui::Text("%zu items selected", m_selectedIndices.size());
        if (modelCount > 0)
        {
            ImGui::Text("  Models:  %zu", modelCount);
        }
        if (dirCount > 0)
        {
            ImGui::Text("  Folders: %zu", dirCount);
        }
        if (otherCount > 0)
        {
            ImGui::Text("  Other:   %zu", otherCount);
        }
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

    if (!entry.isDir && AssetRegistry::IsRegistrableAsset(entry.path))
    {
        const std::string uuid = AssetRegistry::GetOrCreateUuid(entry.path);
        if (!uuid.empty())
        {
            ImGui::TextDisabled("UUID: %s", uuid.c_str());
        }
    }

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
            for (const Entry* e : selected)
            {
                if (e->type == AssetType::Model)
                {
                    result.batchLoadModelPaths.push_back(e->path.string());
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
        m_pendingDeletePaths.clear();
        m_pendingDeleteHasDir = false;
        for (const Entry* e : selected)
        {
            m_pendingDeletePaths.push_back(e->path.string());
            m_pendingDeleteHasDir = m_pendingDeleteHasDir || e->isDir;
        }
        BuildPendingDeleteWarnings();
        m_openDeleteModal = true;
    }
}

void AssetManager::DrawEntryContextMenu(const Entry& entry, int index, AssetManagerResult& result)
{
    if (entry.type == AssetType::Model)
    {
        if (ImGui::MenuItem("Load Model"))
        {
            result.selectedModelPath = entry.path.string();
        }
        ImGui::Separator();
    }

    if (entry.name != ".." && ImGui::MenuItem("Rename", "F2"))
    {
        BeginRename(index);
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
                m_currentDir.string()};
        }
    }

    if (entry.name != "..")
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            m_pendingDeletePaths.clear();
            m_pendingDeletePaths.push_back(entry.path.string());
            m_pendingDeleteHasDir = entry.isDir;
            BuildPendingDeleteWarnings();
            m_openDeleteModal = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Rename / new folder

void AssetManager::BeginRename(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
    {
        return;
    }
    const Entry& entry = m_entries[static_cast<size_t>(index)];
    if (entry.name == "..")
    {
        return;
    }

    m_renamingIndex = index;
    m_renameFocusPending = true;
    std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", entry.name.c_str());

    m_selectedIndices.clear();
    m_selectedIndices.insert(index);
    m_anchorIdx = index;
}

void AssetManager::CommitRename()
{
    const int index = m_renamingIndex;
    m_renamingIndex = -1;
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
    {
        return;
    }
    const Entry& entry = m_entries[static_cast<size_t>(index)];

    std::string newName(m_renameBuffer);
    const size_t first = newName.find_first_not_of(" \t");
    const size_t last = newName.find_last_not_of(" \t");
    newName = (first == std::string::npos) ? std::string{} : newName.substr(first, last - first + 1);

    if (newName.empty() || newName == entry.name ||
        newName.find_first_of("\\/:*?\"<>|") != std::string::npos)
    {
        return;
    }

    const std::filesystem::path target = entry.path.parent_path() / newName;
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        return; // never clobber an existing file/folder
    }
    std::filesystem::rename(entry.path, target, ec);
    if (!ec)
    {
        // Keep the uuid registry and companion sidecars pointing at the new name.
        AssetRegistry::OnAssetRenamed(entry.path, target);
        if (!entry.isDir)
        {
            RenameModelMaterialSidecars(entry.path, target);
        }
    }
    m_needsScan = true;
}

void AssetManager::CancelRename()
{
    m_renamingIndex = -1;
}

void AssetManager::CreateNewFolder()
{
    std::error_code ec;
    std::filesystem::path target = m_currentDir / "NewFolder";
    int suffix = 1;
    while (std::filesystem::exists(target, ec))
    {
        target = m_currentDir / ("NewFolder" + std::to_string(suffix++));
    }

    std::filesystem::create_directory(target, ec);
    if (!ec)
    {
        m_pendingRenameName = target.filename().string();
        m_needsScan = true;
    }
}

void AssetManager::BuildPendingDeleteWarnings()
{
    m_pendingDeleteWarnings.clear();

    // Names of every file that would disappear, including files inside
    // folders staged for deletion, plus the set of their paths so the files
    // being deleted are not counted as referencing each other.
    std::vector<std::string> deletedNames;
    std::unordered_set<std::string> deletedPaths;
    constexpr size_t kMaxNames = 256;
    std::error_code ec;
    for (const std::string& pendingPath : m_pendingDeletePaths)
    {
        const std::filesystem::path p(pendingPath);
        deletedPaths.insert(p.lexically_normal().string());
        if (std::filesystem::is_directory(p, ec))
        {
            for (const auto& item : std::filesystem::recursive_directory_iterator(
                     p, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                if (ec || deletedNames.size() >= kMaxNames)
                {
                    break;
                }
                if (!item.is_regular_file(ec))
                {
                    continue;
                }
                deletedPaths.insert(item.path().lexically_normal().string());
                deletedNames.push_back(item.path().filename().string());
            }
        }
        else
        {
            deletedNames.push_back(p.filename().string());
        }
        if (deletedNames.size() >= kMaxNames)
        {
            break;
        }
    }
    if (deletedNames.empty())
    {
        return;
    }

    // Look through the files that can hold references (.gltf URIs, material /
    // scene YAML paths) for mentions of any doomed file name. Substring search
    // on the filename is a heuristic — it can flag a same-named file in another
    // folder — but a spurious warning is cheap next to a silently broken
    // reference.
    constexpr std::uintmax_t kMaxScanFileBytes = 64ull * 1024 * 1024;
    constexpr size_t kMaxWarnings = 6;
    for (const auto& item : std::filesystem::recursive_directory_iterator(
             m_root, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec || m_pendingDeleteWarnings.size() >= kMaxWarnings)
        {
            break;
        }
        if (!item.is_regular_file(ec))
        {
            continue;
        }
        const std::string ext = ToLower(item.path().extension().string());
        if (ext != ".gltf" && ext != ".yaml" && ext != ".yml")
        {
            continue;
        }
        // Uuid sidecars name their own asset by design; they are not references.
        if (IsHiddenAsset(item.path()))
        {
            continue;
        }
        if (deletedPaths.count(item.path().lexically_normal().string()) > 0)
        {
            continue;
        }
        std::error_code sizeEc;
        if (std::filesystem::file_size(item.path(), sizeEc) > kMaxScanFileBytes || sizeEc)
        {
            continue;
        }

        std::ifstream file(item.path(), std::ios::binary);
        if (!file)
        {
            continue;
        }
        const std::string content(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        for (const std::string& name : deletedNames)
        {
            if (content.find(name) == std::string::npos)
            {
                continue;
            }
            m_pendingDeleteWarnings.push_back(
                "'" + name + "' is referenced by " + item.path().filename().string());
            if (m_pendingDeleteWarnings.size() >= kMaxWarnings)
            {
                break;
            }
        }
    }
}

void AssetManager::DrawDeleteConfirmModal(AssetManagerResult& result)
{
    constexpr const char* kTitle = "Delete Assets?";

    if (m_openDeleteModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openDeleteModal = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Permanently delete %zu item(s)?", m_pendingDeletePaths.size());
        ImGui::Spacing();

        constexpr size_t kMaxListed = 8;
        for (size_t i = 0; i < m_pendingDeletePaths.size() && i < kMaxListed; ++i)
        {
            const std::filesystem::path p(m_pendingDeletePaths[i]);
            ImGui::BulletText("%s", p.filename().string().c_str());
        }
        if (m_pendingDeletePaths.size() > kMaxListed)
        {
            ImGui::TextDisabled("...and %zu more", m_pendingDeletePaths.size() - kMaxListed);
        }

        ImGui::Spacing();
        if (m_pendingDeleteHasDir)
        {
            ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.35f, 1.0f), "Folders are deleted recursively.");
        }
        for (const std::string& warning : m_pendingDeleteWarnings)
        {
            ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.35f, 1.0f), "%s", warning.c_str());
        }
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
        {
            for (std::string& path : m_pendingDeletePaths)
            {
                result.deleteRequests.push_back(std::move(path));
            }
            m_pendingDeletePaths.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_pendingDeletePaths.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
    else if (!m_pendingDeletePaths.empty())
    {
        // Modal was dismissed without an explicit choice (e.g. Escape): treat as cancel.
        m_pendingDeletePaths.clear();
    }
}
}
