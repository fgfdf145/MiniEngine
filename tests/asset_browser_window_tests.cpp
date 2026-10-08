// The Assets window at three widths, drawn without a GPU: the toolbar and the breadcrumb wrap
// instead of running past the right edge, the tiles fill each row, and the preview panel wraps
// a long path. With MINIENGINE_UI_SNAPSHOT_DIR set the picture is written there as a PNG.
// Then tiles dragged onto folders, "..", and a clashing name, folders that spring open under a
// resting drag, and the folder tree, with the mouse driven frame by frame.

#include <engine/asset/asset_manager.h>
#include <engine/asset/asset_registry.h>
#include <engine/editor/editor_icons.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "imgui_software_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me;
using namespace me::test;

namespace
{
constexpr int kWidth = 1240;
constexpr int kHeight = 560;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

void Touch(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

struct BrowserWindow
{
    const char* name;
    ImVec2 pos;
    ImVec2 size;
    std::unique_ptr<AssetManager> manager;
};

// One frame of every window, with the mouse where `mouse` says.
ImDrawData* DrawFrame(std::vector<BrowserWindow>& windows, ImVec2 mouse, bool mouseDown)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    for (BrowserWindow& window : windows)
    {
        ImGui::SetNextWindowPos(window.pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(window.size, ImGuiCond_Always);
        if (ImGui::Begin(window.name, nullptr, ImGuiWindowFlags_NoSavedSettings))
        {
            window.manager->Draw();
        }
        ImGui::End();
    }
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    return drawData;
}

// The child window of `parent` whose name ends with `suffix`.
ImGuiWindow* FindChild(const char* parent, const char* suffix)
{
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        const std::string name = window->Name;
        if (name.starts_with(std::string(parent) + "/") && name.find(suffix) != std::string::npos)
        {
            return window;
        }
    }
    return nullptr;
}

void TestTheWindowFollowsItsWidth()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "miniengine_asset_browser_window_test" / "assets";
    std::filesystem::remove_all(root.parent_path());
    // Deep enough that the breadcrumb cannot fit the narrow window on one line.
    const std::filesystem::path deep = root / "vehicles" / "skyline_r34_vspec" / "textures_and_materials";
    Touch(deep / "a_really_long_model_name_that_wraps.glb");
    Touch(deep / "body.material.yaml");
    Touch(deep / "grove_street.yaml");
    Touch(deep / "body_diffuse.png");
    Touch(deep / "body.bin");
    std::filesystem::create_directories(deep / "lod");
    AssetRegistry::Initialize(root);

    std::vector<BrowserWindow> windows;
    windows.push_back({"Narrow", ImVec2(10.0f, 10.0f), ImVec2(230.0f, 540.0f), std::make_unique<AssetManager>(root)});
    windows.push_back({"Medium", ImVec2(250.0f, 10.0f), ImVec2(400.0f, 540.0f), std::make_unique<AssetManager>(root)});
    windows.push_back({"Wide", ImVec2(660.0f, 10.0f), ImVec2(570.0f, 540.0f), std::make_unique<AssetManager>(root)});
    for (BrowserWindow& window : windows)
    {
        window.manager->NavigateTo(deep);
    }

    const ImVec2 away(-100.0f, -100.0f);
    for (int frame = 0; frame < 3; ++frame)
    {
        DrawFrame(windows, away, false);
    }
    // Select the long-named model in each window: it is the second tile (the folder comes first).
    for (BrowserWindow& window : windows)
    {
        ImGuiWindow* list = FindChild(window.name, "##asset_list");
        Require(list != nullptr, std::string("the ") + window.name + " window has a tile list");
        const ImGuiStyle& style = ImGui::GetStyle();
        const float rowWidth = list->ContentRegionRect.GetWidth();
        const int columns = std::max(1, static_cast<int>((rowWidth + style.ItemSpacing.x) / (96.0f + style.ItemSpacing.x)));
        const float tileWidth = (rowWidth - style.ItemSpacing.x * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const ImVec2 tile = columns > 1 ? ImVec2(list->ContentRegionRect.Min.x + tileWidth * 1.5f + style.ItemSpacing.x,
                                                 list->ContentRegionRect.Min.y + 50.0f)
                                        : ImVec2(list->ContentRegionRect.Min.x + tileWidth * 0.5f,
                                                 list->ContentRegionRect.Min.y + 100.0f + style.ItemSpacing.y + 50.0f);
        DrawFrame(windows, tile, false);
        DrawFrame(windows, tile, true);
        DrawFrame(windows, tile, false);
    }
    ImDrawData* drawData = nullptr;
    for (int frame = 0; frame < 3; ++frame)
    {
        drawData = DrawFrame(windows, away, false);
    }

    if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
    {
        std::filesystem::create_directories(folder);
        WritePng(Rasterise(*drawData, kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / "asset_browser_widths.png");
    }
    for (const BrowserWindow& window : windows)
    {
        ImGuiWindow* imguiWindow = ImGui::FindWindowByName(window.name);
        Require(imguiWindow != nullptr, std::string("the ") + window.name + " window exists");
        const float overflow = imguiWindow->ContentSizeIdeal.x - imguiWindow->ContentRegionRect.GetWidth();
        std::cout << window.name << ": content " << imguiWindow->ContentSizeIdeal.x << " px in "
                  << imguiWindow->ContentRegionRect.GetWidth() << " px\n";
        Require(overflow <= 1.0f, std::string("nothing in the ") + window.name + " window runs past its right edge");
        Require(!imguiWindow->ScrollbarX && !imguiWindow->ScrollbarY, std::string("the ") + window.name + " window needs no scroll bar");

        // A selectable's hit box reaches half the item spacing past the tile, which ImGui counts
        // as content: the last tile of a row may stick out by that much and no more.
        ImGuiWindow* list = FindChild(window.name, "##asset_list");
        Require(list != nullptr, std::string("the ") + window.name + " window has a tile list");
        std::cout << "  tiles " << list->ContentSizeIdeal.x << " px in " << list->ContentRegionRect.GetWidth() << " px\n";
        Require(list->ContentSizeIdeal.x <= list->ContentRegionRect.GetWidth() + ImGui::GetStyle().ItemSpacing.x * 0.5f + 1.0f,
                std::string("the ") + window.name + " tiles fit their row");
        ImGuiWindow* preview = FindChild(window.name, "##asset_preview");
        Require(preview != nullptr && preview->ContentSizeIdeal.x <= preview->ContentRegionRect.GetWidth() + 1.0f,
                std::string("the ") + window.name + " preview wraps its text");
    }

    std::filesystem::remove_all(root.parent_path());
}

// One frame of a single browser, returning what it reported.
AssetManagerResult DrawBrowserFrame(AssetManager& manager, ImVec2 mouse, bool mouseDown)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    AssetManagerResult result;
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(570.0f, 540.0f), ImGuiCond_Always);
    if (ImGui::Begin("Drag", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        result = manager.Draw();
    }
    ImGui::End();
    ImGui::Render();
    ServeTextures(*ImGui::GetDrawData());
    return result;
}

void SnapshotIfAsked(const char* fileName)
{
    if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
    {
        std::filesystem::create_directories(folder);
        WritePng(Rasterise(*ImGui::GetDrawData(), kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / fileName);
    }
}

// The centre of tile `index` in the "Drag" window's list.
ImVec2 TileCentre(int index)
{
    ImGuiWindow* list = FindChild("Drag", "##asset_list");
    Require(list != nullptr, "the drag window has a tile list");
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowWidth = list->ContentRegionRect.GetWidth();
    const int columns = std::max(1, static_cast<int>((rowWidth + style.ItemSpacing.x) / (96.0f + style.ItemSpacing.x)));
    const float tileWidth = (rowWidth - style.ItemSpacing.x * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const int column = index % columns;
    const int row = index / columns;
    return ImVec2(list->ContentRegionRect.Min.x + static_cast<float>(column) * (tileWidth + style.ItemSpacing.x) + tileWidth * 0.5f,
                  list->ContentRegionRect.Min.y + static_cast<float>(row) * (100.0f + style.ItemSpacing.y) + 50.0f);
}

// The middle of row `row` in the "Drag" window's folder tree.
ImVec2 TreeRow(int row)
{
    ImGuiWindow* tree = FindChild("Drag", "##asset_tree");
    Require(tree != nullptr, "the drag window shows the folder tree");
    // Near the row's right end: nested rows have their expand arrow further left.
    return ImVec2(tree->ContentRegionRect.Max.x - 20.0f,
                  tree->ContentRegionRect.Min.y + static_cast<float>(row) * ImGui::GetTextLineHeightWithSpacing() +
                      ImGui::GetTextLineHeight() * 0.5f);
}

// Empty space at the bottom of the tile list.
ImVec2 EmptyListSpot()
{
    ImGuiWindow* list = FindChild("Drag", "##asset_list");
    Require(list != nullptr, "the drag window has a tile list");
    return ImVec2(list->ContentRegionRect.GetCenter().x, list->ContentRegionRect.Max.y - 10.0f);
}

// Where a drag goes next, looked up once the mouse heads there (the layout can change on the
// way), and how many frames it rests there.
struct Waypoint
{
    std::function<ImVec2()> where;
    int restFrames = 1;
    const char* snapshot = nullptr; // taken at the end of the rest
};

// Presses at `from`, drags through the waypoints and lets go at the last; returns what the
// browser reported over the whole drag.
std::vector<AssetManagerResult::RenamedAsset> Drag(AssetManager& manager, const std::function<ImVec2()>& from,
                                                   const std::vector<Waypoint>& waypoints)
{
    const ImVec2 away(-100.0f, -100.0f);
    for (int frame = 0; frame < 3; ++frame)
    {
        DrawBrowserFrame(manager, away, false);
    }
    std::vector<AssetManagerResult::RenamedAsset> renamed;
    const auto collect = [&](const AssetManagerResult& result)
    {
        renamed.insert(renamed.end(), result.renamedAssets.begin(), result.renamedAssets.end());
    };
    ImVec2 mouse = from();
    collect(DrawBrowserFrame(manager, mouse, false));
    collect(DrawBrowserFrame(manager, mouse, true));
    for (const Waypoint& waypoint : waypoints)
    {
        const ImVec2 start = mouse;
        const ImVec2 end = waypoint.where();
        constexpr int kSteps = 6;
        for (int step = 1; step <= kSteps; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(kSteps);
            mouse = ImVec2(start.x + (end.x - start.x) * t, start.y + (end.y - start.y) * t);
            collect(DrawBrowserFrame(manager, mouse, true));
        }
        for (int frame = 0; frame < waypoint.restFrames; ++frame)
        {
            collect(DrawBrowserFrame(manager, mouse, true));
        }
        if (waypoint.snapshot != nullptr)
        {
            SnapshotIfAsked(waypoint.snapshot);
        }
    }
    collect(DrawBrowserFrame(manager, mouse, false));
    for (int frame = 0; frame < 2; ++frame)
    {
        collect(DrawBrowserFrame(manager, away, false));
    }
    return renamed;
}

// Drags tile `from` onto tile `to`, resting there only briefly: no folder springs open.
std::vector<AssetManagerResult::RenamedAsset> DragTile(AssetManager& manager, int from, int to,
                                                       const char* hoverSnapshot = nullptr)
{
    return Drag(manager, [from] { return TileCentre(from); }, {Waypoint{[to] { return TileCentre(to); }, 1, hoverSnapshot}});
}

void Click(AssetManager& manager, ImVec2 where)
{
    DrawBrowserFrame(manager, where, false);
    DrawBrowserFrame(manager, where, true);
    DrawBrowserFrame(manager, where, false);
    DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);
}

// A drag at rest opens a folder after 0.7 s; frames here are 1/60 s.
constexpr int kSpringFrames = 50;

void TestDraggingMovesFolders()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "miniengine_asset_browser_drag_test" / "assets";
    std::filesystem::remove_all(root.parent_path());
    Touch(root / "alpha" / "inside" / "rock.png");
    std::filesystem::create_directories(root / "bravo");
    Touch(root / "charlie" / "alpha" / "keep.txt");
    Touch(root / "tree.glb");
    AssetRegistry::Initialize(root);

    AssetManager manager(root);
    // Root tiles: alpha, bravo, charlie, tree.glb.
    DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);

    // A folder dragged onto a sibling folder goes inside it, with everything it holds.
    std::vector<AssetManagerResult::RenamedAsset> renamed = DragTile(manager, 0, 1, "asset_browser_drag_hover.png");
    Require(std::filesystem::exists(root / "bravo" / "alpha" / "inside" / "rock.png"), "alpha moved into bravo");
    Require(!std::filesystem::exists(root / "alpha"), "alpha left the root");
    Require(renamed.size() == 1 && std::filesystem::path(renamed[0].newPath) == root / "bravo" / "alpha",
            "the move is reported so open scenes follow it");
    Require(AssetRegistry::ResolveUuid(AssetRegistry::GetOrCreateUuid(root / "bravo" / "alpha" / "inside" / "rock.png")).has_value(),
            "the registry knows the moved texture");

    // A model keeps its viewport payload and still moves into a folder. Root: bravo, charlie, tree.glb.
    renamed = DragTile(manager, 2, 0);
    Require(std::filesystem::exists(root / "bravo" / "tree.glb") && !std::filesystem::exists(root / "tree.glb"),
            "the model moved into bravo");

    // Onto ".." moves a folder up a level. Inside bravo: .., alpha, tree.glb.
    manager.NavigateTo(root / "bravo");
    DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);
    renamed = DragTile(manager, 1, 0);
    Require(std::filesystem::exists(root / "alpha" / "inside" / "rock.png") && !std::filesystem::exists(root / "bravo" / "alpha"),
            "dropping on '..' moved alpha back to the root");

    // A folder that already holds the name refuses the move instead of clobbering it.
    // Root: alpha, bravo, charlie.
    manager.NavigateTo(root);
    DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);
    renamed = DragTile(manager, 0, 2);
    Require(renamed.empty(), "a clashing move does nothing");
    Require(std::filesystem::exists(root / "alpha" / "inside" / "rock.png") &&
                std::filesystem::exists(root / "charlie" / "alpha" / "keep.txt") &&
                !std::filesystem::exists(root / "charlie" / "alpha" / "inside"),
            "neither folder changed");

    // A .gltf whose buffer stays behind asks first. Root: alpha, bravo, charlie, scene.bin, scene.gltf.
    std::ofstream(root / "scene.gltf") << R"({"buffers":[{"uri":"scene.bin"}]})";
    Touch(root / "scene.bin");
    manager.Refresh();
    DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);
    renamed = DragTile(manager, 4, 1);
    SnapshotIfAsked("asset_browser_move_confirm.png");
    Require(renamed.empty() && std::filesystem::exists(root / "scene.gltf"), "the move waits for the confirmation");
    ImGuiWindow* modal = ImGui::FindWindowByName("Move Referenced Assets?");
    Require(modal != nullptr && modal->Active, "the confirmation is open");

    // "Move Anyway", the first button on the modal's last row, goes ahead.
    const ImGuiStyle& style = ImGui::GetStyle();
    Click(manager, ImVec2(modal->Pos.x + style.WindowPadding.x + 20.0f,
                          modal->Pos.y + modal->Size.y - style.WindowPadding.y - ImGui::GetFrameHeight() * 0.5f));
    Require(std::filesystem::exists(root / "bravo" / "scene.gltf") && !std::filesystem::exists(root / "scene.gltf"),
            "confirming moved the .gltf");
    Require(!modal->Active, "the confirmation closed");

    std::filesystem::remove_all(root.parent_path());
}

bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec);
}

void TestSpringLoadingAndTheFolderTree()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "miniengine_asset_browser_tree_test" / "assets";
    std::filesystem::remove_all(root.parent_path());
    Touch(root / "alpha" / "a.png");
    std::filesystem::create_directories(root / "bravo" / "deep");
    std::filesystem::create_directories(root / "charlie");
    AssetRegistry::Initialize(root);

    AssetManager manager(root);
    DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);

    // Resting on bravo opens it; the drag carries on (its preview too, though alpha's tile is
    // gone) and drops into bravo through the list's empty space.
    std::vector<AssetManagerResult::RenamedAsset> renamed = Drag(
        manager, []
        { return TileCentre(0); },
        {Waypoint{[]
                  { return TileCentre(1); },
                  kSpringFrames},
         Waypoint{EmptyListSpot, 2, "asset_browser_spring.png"}});
    Require(SameFolder(manager.GetCurrentDirectory(), root / "bravo"), "resting on bravo opened it");
    Require(renamed.size() == 1 && std::filesystem::exists(root / "bravo" / "alpha" / "a.png"),
            "the drag dropped into the folder it sprang into");

    // Tree rows: assets, bravo, charlie. Clicking a row shows that folder.
    Click(manager, TreeRow(2));
    Require(SameFolder(manager.GetCurrentDirectory(), root / "charlie"), "clicking charlie in the tree shows it");

    // Dragging charlie in the tree and resting on bravo expands bravo (rows: assets, bravo,
    // alpha, deep, charlie); dropping on deep moves charlie there, and the list follows it.
    renamed = Drag(
        manager, []
        { return TreeRow(2); },
        {Waypoint{[]
                  { return TreeRow(1); },
                  kSpringFrames},
         Waypoint{[]
                  { return TreeRow(3); },
                  2}});
    Require(renamed.size() == 1 && std::filesystem::exists(root / "bravo" / "deep" / "charlie"),
            "charlie moved onto the folder bravo expanded to show");
    Require(SameFolder(manager.GetCurrentDirectory(), root / "bravo" / "deep" / "charlie"),
            "the list kept showing charlie where it went");
    for (int frame = 0; frame < 3; ++frame)
    {
        DrawBrowserFrame(manager, ImVec2(-100.0f, -100.0f), false);
    }
    SnapshotIfAsked("asset_browser_tree.png");
    // Revealed in the tree: assets, bravo, alpha, deep, charlie. Its row is the one selected.
    Click(manager, TreeRow(3));
    Require(SameFolder(manager.GetCurrentDirectory(), root / "bravo" / "deep"), "deep is the tree's fourth row");
    Click(manager, TreeRow(4));
    Require(SameFolder(manager.GetCurrentDirectory(), root / "bravo" / "deep" / "charlie"),
            "the tree opened deep to show where charlie went");

    std::filesystem::remove_all(root.parent_path());
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // An explicit size: ImGui 1.92.9 refuses to merge the sized icon font into an implicitly sized one.
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 13.0f;
    io.Fonts->AddFontDefault(&fontConfig);
    // Anti-aliased lines as geometry with a fading fringe, which the software rasteriser
    // reproduces; the textured kind needs the GPU's filtering.
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
    MergeEditorIconFont(*io.Fonts, 13.0f);
    int result = 0;
    try
    {
        TestTheWindowFollowsItsWidth();
        TestDraggingMovesFolders();
        TestSpringLoadingAndTheFolderTree();
        std::cout << "asset browser window tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset browser window tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
