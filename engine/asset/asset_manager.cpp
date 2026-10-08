#include "asset_manager.h"

#include "asset_paths.h"
#include "asset_references.h"
#include "asset_registry.h"
#include "material_definition.h"
#include "model_cache.h"
#include "svg_icon.h"

#include <engine/audio/audio_file.h>
#include <engine/core/text/ascii.h>
#include <engine/editor/ui_colors.h>

#include <IconsPhosphor.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace me
{

// The type icons' SVG documents, embedded by engine/asset/CMakeLists.txt.
extern const unsigned char kAssetIconSvg_folder[];
extern const std::size_t kAssetIconSvg_folderSize;
extern const unsigned char kAssetIconSvg_model[];
extern const std::size_t kAssetIconSvg_modelSize;
extern const unsigned char kAssetIconSvg_material[];
extern const std::size_t kAssetIconSvg_materialSize;
extern const unsigned char kAssetIconSvg_scene[];
extern const std::size_t kAssetIconSvg_sceneSize;
extern const unsigned char kAssetIconSvg_texture[];
extern const std::size_t kAssetIconSvg_textureSize;
extern const unsigned char kAssetIconSvg_file[];
extern const std::size_t kAssetIconSvg_fileSize;
extern const unsigned char kAssetIconSvg_parent_folder[];
extern const std::size_t kAssetIconSvg_parent_folderSize;
extern const unsigned char kAssetIconSvg_audio[];
extern const std::size_t kAssetIconSvg_audioSize;

namespace
{
bool IsModelExt(const std::filesystem::path& p)
{
    const std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".gltf" || ext == ".glb";
}

bool IsMaterialFile(const std::filesystem::path& p)
{
    return p.filename().string().ends_with(".material.yaml");
}

bool IsSceneFile(const std::filesystem::path& p)
{
    const std::string ext = ToLowerAscii(p.extension().string());
    if (ext != ".yaml" && ext != ".yml")
    {
        return false;
    }
    const std::string name = p.filename().string();
    return !name.ends_with(".material.yaml") && !name.ends_with(".miniengine_asset.yaml");
}

bool IsTextureExt(const std::filesystem::path& p)
{
    const std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".exr" || ext == ".dds" ||
           ext == ".ktx2";
}

bool IsHiddenAsset(const std::filesystem::path& p)
{
    return p.filename().string().ends_with(".miniengine_asset.yaml");
}

// A renamed or moved model keeps its "<stem>_<index>.material.yaml" sidecars
// attached by renaming them to the new stem beside the new file.
void RenameModelMaterialSidecars(const std::filesystem::path& oldModelPath, const std::filesystem::path& newModelPath)
{
    if (!IsModelExt(oldModelPath))
    {
        return;
    }

    const std::string oldPrefix = oldModelPath.stem().string() + "_";
    const std::string newPrefix = newModelPath.stem().string() + "_";
    for (const std::filesystem::path& definition : FindMaterialDefinitionFiles(oldModelPath))
    {
        const std::string name = definition.filename().string();
        std::error_code renameEc;
        std::filesystem::rename(
            definition,
            newModelPath.parent_path() / (newPrefix + name.substr(oldPrefix.size())),
            renameEc);
    }
}

// Drag payloads. A model keeps the type the viewport accepts, so the same drag can place it
// in the scene or move it into a folder; every other tile carries the generic one.
constexpr const char* kModelPayload = "ASSET_MODEL_PATH";
constexpr const char* kEntryPayload = "ASSET_ENTRY_PATH";

std::string PayloadString(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize <= 0)
    {
        return {};
    }
    return std::string(static_cast<const char*>(payload.Data), static_cast<size_t>(payload.DataSize - 1));
}

bool IsSamePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return AssetPaths::IsSameOrInside(a, b) && AssetPaths::IsSameOrInside(b, a);
}

// A moved .gltf finds its .bin and textures by relative path: name the files beside it that
// it mentions and that stay behind.
void AppendLeftBehindWarnings(
    const std::filesystem::path& gltfPath,
    const std::unordered_set<std::string>& movingPaths,
    std::vector<std::string>& warnings)
{
    if (ToLowerAscii(gltfPath.extension().string()) != ".gltf")
    {
        return;
    }
    constexpr std::uintmax_t kMaxBytes = 64ull * 1024 * 1024;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(gltfPath, ec);
    if (ec || size > kMaxBytes)
    {
        return;
    }
    std::ifstream stream(gltfPath, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};

    for (std::filesystem::directory_iterator it(gltfPath.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path sibling = it->path();
        if (movingPaths.count(sibling.lexically_normal().string()) > 0 || IsHiddenAsset(sibling))
        {
            continue;
        }
        // A uri starts with the name ("scene.bin") or runs through it ("textures/body.png").
        const std::string name = sibling.filename().string();
        if (content.find('"' + name) != std::string::npos || content.find(name + '/') != std::string::npos)
        {
            warnings.push_back("'" + gltfPath.filename().string() + "' uses '" + name + "', which stays behind");
        }
    }
}

// The editor keeps FontScaleMain at its effective UI scale (window DPI times the user's
// multiplier), so pixel sizes here are multiplied by it to stay in proportion with the text.
float UiScale()
{
    return ImGui::GetStyle().FontScaleMain;
}

// Square tile layout (Unreal-style content browser), sized at UI scale 1. Tiles are at least
// this wide; the list stretches them so each row fills the window.
float MinTileWidth()
{
    return 96.0f * UiScale();
}

float TileIconSize()
{
    return 44.0f * UiScale();
}

float TileIconHeight()
{
    return 64.0f * UiScale();
}

float TileHeight()
{
    return 100.0f * UiScale(); // icon area + ~2 lines of label
}

float MinTreeWidth()
{
    return 120.0f * UiScale();
}

float DefaultTreeWidth()
{
    return 180.0f * UiScale();
}

// The widest the folder tree may get in a browser `width` wide: the tiles keep room for three
// columns. Under MinTreeWidth() there is no room for the tree at all.
float MaxTreeWidth(float width)
{
    return width - 3.0f * MinTileWidth() - ImGui::GetStyle().ItemSpacing.x;
}

// How long a drag rests on a folder before it opens (Unreal and Explorer wait about as long).
constexpr double kSpringLoadSeconds = 0.7;

// Whether an item `width` wide still fits on the current line after the last item.
bool FitsOnLine(float width, float rightEdge)
{
    return ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= rightEdge;
}

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// A toolbar button that moves to the next line when the window is too narrow for it.
bool WrappingButton(const char* label, float rightEdge, bool first)
{
    if (!first && FitsOnLine(ButtonWidth(label), rightEdge))
    {
        ImGui::SameLine();
    }
    return ImGui::Button(label);
}
}

// ---------------------------------------------------------------------------

AssetManager::AssetManager(std::filesystem::path assetsRoot)
    : m_root(std::move(assetsRoot)), m_currentDir(m_root)
{
}

void AssetManager::Refresh()
{
    m_needsScan = true;
    m_treeChildren.clear();
}

void AssetManager::NavigateTo(const std::filesystem::path& dir)
{
    m_currentDir = dir;
    m_selectedIndices.clear();
    m_anchorIdx = -1;
    m_renamingIndex = -1;
    m_needsScan = true;
    m_treeRevealPending = true;
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
    if (!m_statusError.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui_colors::kTextDanger, "%s", m_statusError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();

    m_springHovered = false;
    // The folder tree on the left, as long as the tiles keep room for three columns next to it.
    const float maxTreeWidth = MaxTreeWidth(ImGui::GetContentRegionAvail().x);
    if (m_showTree && maxTreeWidth >= MinTreeWidth())
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(MinTreeWidth(), 0.0f), ImVec2(maxTreeWidth, FLT_MAX));
        // ResizeX: the user drags its right edge; ImGui keeps the width in imgui.ini.
        if (ImGui::BeginChild("##asset_tree", ImVec2(DefaultTreeWidth(), 0.0f), ImGuiChildFlags_ResizeX))
        {
            DrawFolderTree();
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    if (ImGui::BeginChild("##asset_main", ImVec2(0.0f, 0.0f)))
    {
        DrawBreadcrumb();
        ImGui::Separator();
        DrawEntryList(result);
        DrawPreviewPanel(result);
    }
    ImGui::EndChild();
    if (!m_springHovered)
    {
        m_springPath.clear();
    }
    KeepDragAlive();

    DrawDeleteConfirmModal(result);
    DrawRenameConfirmModal();
    DrawMoveConfirmModal();

    result.renamedAssets = std::move(m_completedRenames);
    m_completedRenames.clear();
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
    m_previewUuidIndex = -1;
    m_previewUuid.clear();

    std::error_code ec;
    if (!std::filesystem::exists(m_currentDir, ec) || !std::filesystem::is_directory(m_currentDir, ec))
    {
        if (m_currentDir != m_root)
        {
            // The folder went away (deleted or renamed outside the editor): show the root instead.
            m_currentDir = m_root;
            m_treeRevealPending = true;
            m_treeChildren.clear();
            ScanCurrentDir();
        }
        else
        {
            m_statusError = "The assets folder does not exist: " + m_root.string();
        }
        return;
    }
    if (m_currentDir == m_root && m_statusError.starts_with("The assets folder does not exist"))
    {
        m_statusError.clear();
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
    if (IsAudioFilePath(p))
        return AssetType::Audio;
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
    case AssetType::Audio:
        return "[SND]";
    default:
        return "[   ]";
    }
}

void AssetManager::PushTypeColor(AssetType t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, TypeColorU32(t));
}

const SvgIcon* AssetManager::TileIcon(const Entry& entry)
{
    const auto parse = [](const unsigned char* data, std::size_t size)
    {
        return SvgIcon::Parse(std::string_view(reinterpret_cast<const char*>(data), size));
    };
    // Parsed once; each keeps the meshes of the sizes it has been drawn at.
    static const std::array<std::optional<SvgIcon>, 8> icons = {
        parse(kAssetIconSvg_folder, kAssetIconSvg_folderSize),
        parse(kAssetIconSvg_model, kAssetIconSvg_modelSize),
        parse(kAssetIconSvg_material, kAssetIconSvg_materialSize),
        parse(kAssetIconSvg_scene, kAssetIconSvg_sceneSize),
        parse(kAssetIconSvg_texture, kAssetIconSvg_textureSize),
        parse(kAssetIconSvg_file, kAssetIconSvg_fileSize),
        parse(kAssetIconSvg_parent_folder, kAssetIconSvg_parent_folderSize),
        parse(kAssetIconSvg_audio, kAssetIconSvg_audioSize),
    };
    size_t index = 5;
    if (entry.name == "..")
    {
        index = 6;
    }
    else
    {
        switch (entry.type)
        {
        case AssetType::Dir:
            index = 0;
            break;
        case AssetType::Model:
            index = 1;
            break;
        case AssetType::Material:
            index = 2;
            break;
        case AssetType::Scene:
            index = 3;
            break;
        case AssetType::Texture:
            index = 4;
            break;
        case AssetType::Audio:
            index = 7;
            break;
        default:
            break;
        }
    }
    return icons[index].has_value() ? &*icons[index] : nullptr;
}

const char* AssetManager::TypeIcon(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:
        return ICON_PH_FOLDER;
    case AssetType::Model:
        return ICON_PH_CUBE;
    case AssetType::Material:
        return ICON_PH_PALETTE;
    case AssetType::Scene:
        return ICON_PH_MOUNTAINS;
    case AssetType::Texture:
        return ICON_PH_IMAGE;
    case AssetType::Audio:
        return ICON_PH_MUSIC_NOTES;
    default:
        return ICON_PH_FILE;
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
    case AssetType::Audio:
        return "SND";
    default:
        return "FILE";
    }
}

// The Claude desktop app's tint colours (--cds-text-tint-*, dark theme), one hue per asset type.
unsigned int AssetManager::TypeColorU32(AssetType t)
{
    switch (t)
    {
    case AssetType::Dir:
        return IM_COL32(219, 147, 0, 255); // yellow
    case AssetType::Model:
        return IM_COL32(109, 167, 236, 255); // blue
    case AssetType::Material:
        return IM_COL32(160, 150, 235, 255); // violet
    case AssetType::Scene:
        return IM_COL32(85, 191, 80, 255); // green
    case AssetType::Texture:
        return IM_COL32(59, 189, 140, 255); // aqua
    case AssetType::Audio:
        return IM_COL32(232, 123, 164, 255); // magenta
    default:
        return IM_COL32(137, 135, 129, 255); // --cds-text-muted
    }
}

// ---------------------------------------------------------------------------
// UI sections

void AssetManager::DrawToolbar(AssetManagerResult& result)
{
    const float width = ImGui::GetContentRegionAvail().x;
    const float rightEdge = ImGui::GetCursorScreenPos().x + width;
    if (WrappingButton(ICON_PH_FILE_ARROW_DOWN " Import Model", rightEdge, true))
    {
        result.wantsImportModel = true;
    }

    if (WrappingButton(ICON_PH_ARROWS_CLOCKWISE " Refresh", rightEdge, false))
    {
        // Also pick up files changed outside the editor (new/copied/moved assets).
        AssetRegistry::RescanAssetTree();
        Refresh();
    }

    if (WrappingButton(ICON_PH_FOLDER_PLUS " New Folder", rightEdge, false))
    {
        CreateNewFolder();
    }

    // Navigate to root shortcut
    if (WrappingButton(ICON_PH_HOUSE " Assets Root", rightEdge, false))
    {
        NavigateTo(m_root);
    }

    // The folder tree's switch, offered only while the window has room for the tree.
    if (MaxTreeWidth(width) >= MinTreeWidth())
    {
        if (m_showTree)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        const bool toggleTree = WrappingButton(ICON_PH_SIDEBAR_SIMPLE " Folders", rightEdge, false);
        if (m_showTree)
        {
            ImGui::PopStyleColor();
        }
        if (toggleTree)
        {
            m_showTree = !m_showTree;
            m_treeRevealPending = true;
        }
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

    // A deep path wraps onto further lines instead of running off the window's right edge.
    const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const ImGuiStyle& style = ImGui::GetStyle();
    for (size_t i = 0; i < segments.size(); ++i)
    {
        const std::string label = segments[i].filename().string().empty()
                                      ? "assets"
                                      : segments[i].filename().string();

        if (i > 0)
        {
            const float labelWidth = ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2.0f;
            const float separatorWidth = ImGui::CalcTextSize(">").x;
            if (FitsOnLine(separatorWidth + style.ItemSpacing.x + labelWidth, rightEdge))
            {
                ImGui::SameLine();
            }
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }

        const bool isCurrent = (i + 1 == segments.size());
        if (isCurrent)
        {
            ImGui::TextUnformatted(label.c_str());
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_colors::kTextAccent);
            const std::string btnId = label + "##bc" + std::to_string(i);
            if (ImGui::SmallButton(btnId.c_str()))
            {
                NavigateTo(segments[i]);
            }
            ImGui::PopStyleColor();
            if (DrawMoveDropTarget(segments[i], true))
            {
                NavigateTo(segments[i]);
            }
        }
    }
}

void AssetManager::DrawEntryList(AssetManagerResult& result)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().y;
    // The preview panel below takes what it needed last frame, but never more than half the
    // space, so a long multi-selection summary scrolls rather than squeezing the list away.
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float previewHeight = std::clamp(
        m_previewContentHeight > 0.0f ? m_previewContentHeight : 100.0f * UiScale(),
        lineHeight,
        std::max(available * 0.5f, lineHeight));
    // Below the list: item spacing, the separator line and its spacing, then the preview.
    const float listHeight = std::max(available - previewHeight - style.ItemSpacing.y * 2.0f - 1.0f, 60.0f * UiScale());
    if (ImGui::BeginChild("##asset_list", ImVec2(0.0f, listHeight), false))
    {
        // As many minimum-width tiles as fit, then stretched to share the row.
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(
            1,
            static_cast<int>((rowWidth + style.ItemSpacing.x) / (MinTileWidth() + style.ItemSpacing.x)));
        m_tileWidth = std::max(
            (rowWidth - style.ItemSpacing.x * static_cast<float>(columns - 1)) / static_cast<float>(columns),
            TileIconSize());

        if (m_entries.empty())
        {
            ImGui::TextDisabled("This folder is empty. Use Import Model, or drop files onto the window.");
        }

        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
        {
            // Each column at its own offset: a selectable pads its tile's group by half the item
            // spacing, so plain SameLine() would push every column a few pixels further right.
            if (const int column = i % columns; column != 0)
            {
                ImGui::SameLine(static_cast<float>(column) * (m_tileWidth + style.ItemSpacing.x));
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
    // The list's empty space (and its file tiles) stand for the folder it shows, so a drag
    // that sprang into a folder can be dropped there. Folder tiles, being smaller, win.
    DrawMoveDropTarget(m_currentDir, false);
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
                              ImVec2(m_tileWidth, TileHeight())))
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
            else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.type == AssetType::Scene)
            {
                result.openScenePath = entry.path.string();
            }
            else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.type == AssetType::Audio)
            {
                result.previewAudioPath = entry.path.string();
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

            DrawEntryDragSource(entry, index, result);
            // Resting the drag on a folder opens it; the list rescans next frame.
            if (entry.isDir && DrawMoveDropTarget(entry.path, true))
            {
                NavigateTo(entry.path);
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
        ImGui::Dummy(ImVec2(m_tileWidth, TileIconHeight()));
        if (m_renameFocusPending)
        {
            ImGui::SetKeyboardFocusHere();
            m_renameFocusPending = false;
        }
        ImGui::SetNextItemWidth(m_tileWidth);
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

    const float s = UiScale();

    // The type's icon in its colour, centred in the icon area; ".." gets an up arrow. Files
    // also carry their short tag under the icon, at the text's own size: the font atlas is
    // baked at that size, and text drawn larger or smaller than it blurs.
    float iconAreaHeight = TileIconHeight();
    if (!entry.isDir)
    {
        const char* tag = ShortTag(entry.type);
        const ImVec2 tagSize = ImGui::CalcTextSize(tag);
        const float tagTop = tileMin.y + TileIconHeight() - tagSize.y - 2.0f * s;
        drawList->AddText(ImVec2(std::round(tileMin.x + (m_tileWidth - tagSize.x) * 0.5f), tagTop),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), tag);
        iconAreaHeight -= tagSize.y + 2.0f * s;
    }
    const float iconHeight = std::min(TileIconSize(), iconAreaHeight - 8.0f * s);
    if (const SvgIcon* icon = TileIcon(entry); icon != nullptr && iconHeight > 0.0f)
    {
        // Vector icons, sharp at any size.
        const float height = std::min(iconHeight, (m_tileWidth - 8.0f * s) / icon->AspectRatio());
        const float width = height * icon->AspectRatio();
        icon->Draw(*drawList,
                   ImVec2(tileMin.x + (m_tileWidth - width) * 0.5f, tileMin.y + (iconAreaHeight - height) * 0.5f + 2.0f * s),
                   height,
                   typeCol);
    }

    if (!isRenaming)
    {
        // Name label: wrapped to the tile width, clipped to two lines,
        // centered when it fits on one line
        const float labelTop = tileMin.y + TileIconHeight();
        const float wrapWidth = m_tileWidth - 6.0f * s;
        const ImVec2 textSize = ImGui::CalcTextSize(entry.name.c_str(), nullptr, false, wrapWidth);
        const float textX = (textSize.x < wrapWidth)
                                ? tileMin.x + (m_tileWidth - textSize.x) * 0.5f
                                : tileMin.x + 3.0f * s;
        const ImVec4 clipRect(tileMin.x, labelTop, tileMin.x + m_tileWidth, tileMin.y + TileHeight());
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

    // Takes the rest of the window; DrawEntryList sized itself to leave what this needed last frame.
    if (ImGui::BeginChild("##asset_preview", ImVec2(0.0f, 0.0f), false))
    {
        ImGui::PushTextWrapPos(0.0f);
        DrawPreviewDetails(result);
        ImGui::PopTextWrapPos();
        m_previewContentHeight = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;
    }
    ImGui::EndChild();
}

void AssetManager::DrawPreviewDetails(AssetManagerResult& result)
{
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
    ImGui::Text("%s %s", TypeIcon(entry.type), entry.name.c_str());
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
        if (m_previewUuidIndex != focusIdx)
        {
            m_previewUuid = AssetRegistry::GetOrCreateUuid(entry.path);
            m_previewUuidIndex = focusIdx;
        }
        if (!m_previewUuid.empty())
        {
            ImGui::TextDisabled("UUID: %s", m_previewUuid.c_str());
        }
    }

    if (entry.type == AssetType::Model)
    {
        if (ImGui::SmallButton("Load into Scene"))
        {
            result.selectedModelPath = entry.path.string();
        }
    }
    if (entry.type == AssetType::Audio)
    {
        if (ImGui::SmallButton(ICON_PH_PLAY " Play / Stop"))
        {
            result.previewAudioPath = entry.path.string();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("or double-click it");
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
// Folder tree

void AssetManager::DrawFolderTree()
{
    // Taken for this draw: a drop or click in the tree asks again for the next one.
    m_treeRevealing = m_treeRevealPending;
    m_treeRevealPending = false;
    DrawFolderTreeNode(m_root, "assets", true);
}

const std::vector<std::filesystem::path>& AssetManager::TreeChildren(const std::filesystem::path& dir)
{
    const auto [it, inserted] = m_treeChildren.try_emplace(dir.lexically_normal().string());
    if (inserted)
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator entry(dir, ec), end; !ec && entry != end; entry.increment(ec))
        {
            std::error_code typeEc;
            if (entry->is_directory(typeEc))
            {
                it->second.push_back(entry->path());
            }
        }
        std::sort(it->second.begin(), it->second.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
                  {
                      return a.filename().string() < b.filename().string();
                  });
    }
    return it->second;
}

void AssetManager::DrawFolderTreeNode(const std::filesystem::path& dir, const std::string& name, bool isRoot)
{
    // A copy: a drop further down this frame can rebuild the listings.
    const std::vector<std::filesystem::path> children = TreeChildren(dir);
    const bool isCurrent = IsSamePath(dir, m_currentDir);
    const std::string key = dir.lexically_normal().string();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                               ImGuiTreeNodeFlags_NavLeftJumpsToParent;
    if (children.empty())
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (isCurrent)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (isRoot)
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }
    // Opened to reveal the folder the list shows, or by a drag resting on it.
    const bool reveal = m_treeRevealing && !isCurrent && AssetPaths::IsSameOrInside(m_currentDir, dir);
    if (m_treeOpenRequests.erase(key) > 0 || reveal)
    {
        ImGui::SetNextItemOpen(true);
    }
    // The label is drawn after the node so its icon can take the folder colour.
    const bool open = ImGui::TreeNodeEx(name.c_str(), flags, "%s", "");
    if (isCurrent && m_treeRevealing && !ImGui::IsItemVisible())
    {
        ImGui::SetScrollHereY(0.5f);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() && !isCurrent)
    {
        NavigateTo(dir);
    }
    if (!isRoot && ImGui::BeginDragDropSource())
    {
        m_draggedPaths.assign(1, dir.string());
        SubmitDragPayload(dir, AssetType::Dir);
        ImGui::EndDragDropSource();
    }
    // Resting a drag on a folder with subfolders expands it.
    if (DrawMoveDropTarget(dir, !children.empty()))
    {
        m_treeOpenRequests.insert(key);
    }

    ImGui::SameLine();
    PushTypeColor(AssetType::Dir);
    ImGui::TextUnformatted(open && !children.empty() ? ICON_PH_FOLDER_OPEN : ICON_PH_FOLDER);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(name.c_str());

    if (open)
    {
        ImGui::TreePush(name.c_str());
        for (const std::filesystem::path& child : children)
        {
            DrawFolderTreeNode(child, child.filename().string(), false);
        }
        ImGui::TreePop();
    }
}

// ---------------------------------------------------------------------------
// Drag to move

void AssetManager::DrawEntryDragSource(const Entry& entry, int index, AssetManagerResult& result)
{
    if (entry.name == ".." || !ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        return;
    }

    // A selected tile carries the whole selection along; an unselected one goes alone.
    m_draggedPaths.clear();
    if (m_selectedIndices.count(index) > 0)
    {
        std::vector<int> selected(m_selectedIndices.begin(), m_selectedIndices.end());
        std::sort(selected.begin(), selected.end());
        for (const int i : selected)
        {
            if (i >= 0 && i < static_cast<int>(m_entries.size()) && m_entries[static_cast<size_t>(i)].name != "..")
            {
                m_draggedPaths.push_back(m_entries[static_cast<size_t>(i)].path.string());
            }
        }
    }
    else
    {
        m_draggedPaths.push_back(entry.path.string());
    }

    SubmitDragPayload(entry.path, entry.type);
    if (entry.type == AssetType::Model)
    {
        result.draggedModelPath = entry.path.string();
    }
    ImGui::EndDragDropSource();
}

void AssetManager::SubmitDragPayload(const std::filesystem::path& primary, AssetType type)
{
    m_dragPrimaryPath = primary;
    m_dragPrimaryType = type;
    m_dragSubmittedFrame = ImGui::GetFrameCount();

    const std::string pathStr = primary.string();
    ImGui::SetDragDropPayload(type == AssetType::Model ? kModelPayload : kEntryPayload, pathStr.c_str(), pathStr.size() + 1);

    PushTypeColor(type);
    ImGui::TextUnformatted(TypeIcon(type));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const std::string name = primary.filename().string();
    if (m_draggedPaths.size() > 1)
    {
        ImGui::Text("%s and %zu more", name.c_str(), m_draggedPaths.size() - 1);
    }
    else
    {
        ImGui::TextUnformatted(name.c_str());
    }
}

void AssetManager::KeepDragAlive()
{
    // ImGui keeps an orphaned payload while the button is held but previews it as "...".
    if (m_dragSubmittedFrame == ImGui::GetFrameCount() || !ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
        !IsOwnDrag(ImGui::GetDragDropPayload()))
    {
        return;
    }
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
    {
        SubmitDragPayload(m_dragPrimaryPath, m_dragPrimaryType);
        ImGui::EndDragDropSource();
    }
}

bool AssetManager::IsOwnDrag(const ImGuiPayload* payload) const
{
    // A drag started in this browser names one of the paths it carries.
    return payload != nullptr && (payload->IsDataType(kModelPayload) || payload->IsDataType(kEntryPayload)) &&
           std::find(m_draggedPaths.begin(), m_draggedPaths.end(), PayloadString(*payload)) != m_draggedPaths.end();
}

bool AssetManager::DrawMoveDropTarget(const std::filesystem::path& destination, bool springLoaded)
{
    if (!IsOwnDrag(ImGui::GetDragDropPayload()))
    {
        return false;
    }
    // A folder never goes into itself or below itself, and the folder everything already
    // sits in has nothing to receive: neither is a target at all.
    bool allThere = true;
    for (const std::string& dragged : m_draggedPaths)
    {
        if (AssetPaths::IsSameOrInside(destination, dragged))
        {
            return false;
        }
        allThere = allThere && IsSamePath(std::filesystem::path(dragged).parent_path(), destination);
    }
    if (allThere && !springLoaded)
    {
        return false;
    }

    bool springOpen = false;
    if (ImGui::BeginDragDropTarget())
    {
        const ImGuiPayload* payload = nullptr;
        if (!allThere)
        {
            payload = ImGui::AcceptDragDropPayload(kModelPayload);
            if (payload == nullptr)
            {
                payload = ImGui::AcceptDragDropPayload(kEntryPayload);
            }
        }
        if (payload != nullptr)
        {
            RequestMove(m_draggedPaths, destination);
            m_draggedPaths.clear();
            m_springPath.clear();
        }
        else if (springLoaded)
        {
            // Spring-loaded: resting here long enough opens the folder. A bar along the
            // target's bottom edge fills up meanwhile.
            const std::string key = destination.lexically_normal().string();
            const double now = ImGui::GetTime();
            if (m_springPath != key)
            {
                m_springPath = key;
                m_springStart = now;
            }
            m_springHovered = true;
            const double progress = (now - m_springStart) / kSpringLoadSeconds;
            if (progress >= 1.0)
            {
                springOpen = true;
                m_springStart = std::numeric_limits<double>::infinity(); // once per rest
            }
            else if (progress >= 0.0)
            {
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                const float barHeight = 3.0f * UiScale();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(min.x, max.y - barHeight),
                    ImVec2(min.x + (max.x - min.x) * static_cast<float>(progress), max.y),
                    ImGui::GetColorU32(ImGuiCol_DragDropTarget));
            }
        }
        ImGui::EndDragDropTarget();
    }
    return springOpen;
}

void AssetManager::RequestMove(const std::vector<std::string>& sourcePaths, const std::filesystem::path& destination)
{
    m_statusError.clear();
    std::error_code ec;
    PendingMove move;
    move.destinationDirectory = destination.string();
    for (const std::string& sourceString : sourcePaths)
    {
        const std::filesystem::path source(sourceString);
        if (IsSamePath(source.parent_path(), destination))
        {
            continue; // already there
        }
        if (AssetPaths::IsSameOrInside(destination, source))
        {
            m_statusError = "Cannot move '" + source.filename().string() + "' into itself";
            continue;
        }
        if (std::filesystem::exists(destination / source.filename(), ec))
        {
            // Never clobber, as with rename.
            m_statusError = "'" + destination.filename().string() + "' already has a '" +
                            source.filename().string() + "'";
            continue;
        }
        move.sourcePaths.push_back(sourceString);
    }
    if (move.sourcePaths.empty())
    {
        return;
    }

    // Every file that moves, so documents moving together are not counted as referencing
    // each other: their relative paths still hold after the move.
    std::vector<std::string> movedNames;
    std::unordered_set<std::string> movingPaths;
    for (const std::string& sourceString : move.sourcePaths)
    {
        const std::filesystem::path source(sourceString);
        movedNames.push_back(source.filename().string());
        movingPaths.insert(source.lexically_normal().string());
        if (std::filesystem::is_directory(source, ec))
        {
            for (const auto& item : std::filesystem::recursive_directory_iterator(
                     source, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                movingPaths.insert(item.path().lexically_normal().string());
            }
        }
        else
        {
            for (const std::filesystem::path& definition : FindMaterialDefinitionFiles(source))
            {
                movingPaths.insert(definition.lexically_normal().string());
            }
            movingPaths.insert(AssetRegistry::SidecarPathFor(source).lexically_normal().string());
        }
    }

    // As with rename: scenes reference assets by uuid and follow the move, so only glTF
    // files and material definitions, which hold plain paths, break.
    std::vector<std::string> warnings;
    for (const AssetReference& reference : FindReferencesTo(m_root, movedNames, movingPaths))
    {
        if (!IsSceneFile(reference.referencedBy))
        {
            warnings.push_back("'" + reference.referencedName + "' is referenced by " + reference.referencedBy);
        }
    }
    for (const std::string& sourceString : move.sourcePaths)
    {
        AppendLeftBehindWarnings(sourceString, movingPaths, warnings);
    }

    if (!warnings.empty())
    {
        m_pendingMoveWarnings = std::move(warnings);
        m_pendingMove = std::move(move);
        m_openMoveModal = true;
        return; // the modal performs the move on confirmation
    }
    PerformMove(move);
}

void AssetManager::PerformMove(const PendingMove& move)
{
    const std::filesystem::path destination(move.destinationDirectory);
    for (const std::string& sourceString : move.sourcePaths)
    {
        const std::filesystem::path source(sourceString);
        std::error_code ec;
        const bool isDir = std::filesystem::is_directory(source, ec);
        const std::filesystem::path target = destination / source.filename();
        if (!MoveOnDisk(source, target, isDir, ec))
        {
            m_statusError = "Could not move '" + source.filename().string() + "': " + ec.message();
        }
        else if (const std::optional<std::filesystem::path> rebased = AssetPaths::Rebase(m_currentDir, source, target))
        {
            // The folder being shown moved (dragged in the tree): keep showing it.
            m_currentDir = *rebased;
            m_treeRevealPending = true;
        }
    }
    m_needsScan = true;
    m_treeChildren.clear();
}

void AssetManager::DrawMoveConfirmModal()
{
    constexpr const char* kTitle = "Move Referenced Assets?";

    if (m_openMoveModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openMoveModal = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_pendingMove.has_value())
        {
            const std::string destinationName =
                std::filesystem::path(m_pendingMove->destinationDirectory).filename().string();
            if (m_pendingMove->sourcePaths.size() == 1)
            {
                ImGui::Text(
                    "Move '%s' into '%s'?",
                    std::filesystem::path(m_pendingMove->sourcePaths.front()).filename().string().c_str(),
                    destinationName.c_str());
            }
            else
            {
                ImGui::Text("Move %zu items into '%s'?", m_pendingMove->sourcePaths.size(), destinationName.c_str());
            }
        }
        ImGui::Spacing();

        for (const std::string& warning : m_pendingMoveWarnings)
        {
            ImGui::TextColored(ui_colors::kTextWarning, "%s", warning.c_str());
        }
        ImGui::TextDisabled("Those files find each other by relative path: moving breaks them. Scenes are not affected.");
        ImGui::Separator();

        if (ImGui::Button("Move Anyway", ImVec2(140.0f * UiScale(), 0.0f)))
        {
            if (m_pendingMove.has_value())
            {
                PerformMove(*m_pendingMove);
            }
            m_pendingMove.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f * UiScale(), 0.0f)))
        {
            m_pendingMove.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
    else if (m_pendingMove.has_value())
    {
        // Dismissed without an explicit choice (e.g. Escape): treat as cancel.
        m_pendingMove.reset();
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
    // Only the name is edited; the extension is kept, so a rename cannot turn
    // a model into an unknown file type by accident.
    const AssetPaths::RenameableName name = AssetPaths::SplitRenameableName(entry.name, entry.isDir);
    std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", name.editable.c_str());
    m_renameSuffix = name.suffix;

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

    std::string editedName(m_renameBuffer);
    const size_t first = editedName.find_first_not_of(" \t");
    const size_t last = editedName.find_last_not_of(" \t");
    editedName = (first == std::string::npos) ? std::string{} : editedName.substr(first, last - first + 1);
    if (editedName.empty() || editedName.find_first_of("\\/:*?\"<>|") != std::string::npos)
    {
        return;
    }

    const std::string newName = editedName + m_renameSuffix;
    if (newName == entry.name)
    {
        return;
    }

    const std::filesystem::path target = entry.path.parent_path() / newName;
    if (AssetPaths::RenameWouldClobber(entry.path, target))
    {
        return; // never clobber an existing file/folder
    }

    const PendingRename rename{entry.path.string(), newName, entry.isDir};

    // Renaming a file breaks every path-based reference to its old name, the
    // same breakage deleting it causes. Delete warns; rename used to go
    // through silently.
    // Scenes are left out: they reference assets by uuid and open scenes
    // follow the rename, so only glTF files and material definitions, which
    // hold plain paths, actually break.
    std::vector<std::string> warnings;
    for (const AssetReference& reference :
         FindReferencesTo(m_root, {entry.name}, {entry.path.lexically_normal().string()}))
    {
        if (!IsSceneFile(reference.referencedBy))
        {
            warnings.push_back("'" + reference.referencedName + "' is referenced by " + reference.referencedBy);
        }
    }
    if (!warnings.empty())
    {
        m_pendingRenameWarnings = std::move(warnings);
        m_pendingRename = rename;
        m_openRenameModal = true;
        return; // the modal performs the rename on confirmation
    }

    PerformRename(rename);
}

void AssetManager::PerformRename(const PendingRename& rename)
{
    const std::filesystem::path source(rename.sourcePath);
    std::error_code ec;
    MoveOnDisk(source, source.parent_path() / rename.newName, rename.isDir, ec);
    m_needsScan = true;
    m_treeChildren.clear();
}

bool AssetManager::MoveOnDisk(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    bool isDir,
    std::error_code& ec)
{
    std::filesystem::rename(source, target, ec);
    if (ec)
    {
        return false;
    }

    // Parsed model data is keyed on path. Without this, a model re-imported
    // later at the old path is served the previous file's data.
    ModelCache::Invalidate(source.string());

    // Keep the uuid registry and companion sidecars pointing at the new path.
    AssetRegistry::OnAssetRenamed(source, target);
    if (!isDir)
    {
        RenameModelMaterialSidecars(source, target);
    }

    // Reported so open scenes and editors can follow the asset.
    m_completedRenames.push_back(AssetManagerResult::RenamedAsset{source.string(), target.string()});
    return true;
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

    // create_directories also recreates the current folder (or the assets root) if it went missing.
    std::filesystem::create_directories(target, ec);
    if (ec)
    {
        m_statusError = "Could not create folder '" + target.string() + "': " + ec.message();
        return;
    }
    m_statusError.clear();
    m_pendingRenameName = target.filename().string();
    m_needsScan = true;
    m_treeChildren.clear();
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

    // Delegated so the rename flow can ask the same question without paying
    // for a second whole-tree read.
    constexpr size_t kMaxWarnings = 6;
    const std::vector<AssetReference> references =
        FindReferencesTo(m_root, deletedNames, deletedPaths, kMaxWarnings);
    for (const AssetReference& reference : references)
    {
        m_pendingDeleteWarnings.push_back(
            "'" + reference.referencedName + "' is referenced by " + reference.referencedBy);
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
            ImGui::TextColored(ui_colors::kTextWarning, "Folders are deleted recursively.");
        }
        for (const std::string& warning : m_pendingDeleteWarnings)
        {
            ImGui::TextColored(ui_colors::kTextWarning, "%s", warning.c_str());
        }
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button, ui_colors::kFillDanger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_colors::kFillDangerHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui_colors::kFillDanger);
        if (ImGui::Button("Delete", ImVec2(120.0f * UiScale(), 0.0f)))
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
        if (ImGui::Button("Cancel", ImVec2(120.0f * UiScale(), 0.0f)))
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

void AssetManager::DrawRenameConfirmModal()
{
    constexpr const char* kTitle = "Rename Referenced Asset?";

    if (m_openRenameModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openRenameModal = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_pendingRename.has_value())
        {
            ImGui::Text(
                "Rename '%s' to '%s'?",
                std::filesystem::path(m_pendingRename->sourcePath).filename().string().c_str(),
                m_pendingRename->newName.c_str());
        }
        ImGui::Spacing();

        for (const std::string& warning : m_pendingRenameWarnings)
        {
            ImGui::TextColored(ui_colors::kTextWarning, "%s", warning.c_str());
        }
        ImGui::TextDisabled("Those files reference it by path: renaming breaks them. Scenes are not affected.");
        ImGui::Separator();

        if (ImGui::Button("Rename Anyway", ImVec2(140.0f * UiScale(), 0.0f)))
        {
            if (m_pendingRename.has_value())
            {
                PerformRename(*m_pendingRename);
            }
            m_pendingRename.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f * UiScale(), 0.0f)))
        {
            m_pendingRename.reset();
            m_needsScan = true; // refresh the list; the inline edit already closed
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
    else if (m_pendingRename.has_value())
    {
        // Dismissed without an explicit choice (e.g. Escape): treat as cancel.
        m_pendingRename.reset();
        m_needsScan = true;
    }
}
}
