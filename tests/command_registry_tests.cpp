#include <engine/editor/command_registry.h>
#include <engine/editor/editor_commands.h>

#include <imgui.h>

#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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

const CommandMenuNode* FindMenu(const CommandMenuNode& parent, const std::string& label)
{
    for (const CommandMenuNode& child : parent.children)
    {
        if (child.kind == CommandMenuNode::Kind::Menu && child.label == label)
        {
            return &child;
        }
    }
    return nullptr;
}

void CollectItems(const CommandRegistry& registry, const CommandMenuNode& node, int depth, int& maxDepth, std::vector<const Command*>& items)
{
    for (const CommandMenuNode& child : node.children)
    {
        if (child.kind == CommandMenuNode::Kind::Item)
        {
            items.push_back(&registry.GetCommands()[child.commandIndex]);
            maxDepth = depth > maxDepth ? depth : maxDepth;
        }
        else if (child.kind == CommandMenuNode::Kind::Menu)
        {
            CollectItems(registry, child, depth + 1, maxDepth, items);
        }
    }
}

void TestRegistration()
{
    CommandRegistry registry;
    Require(registry.Register(Command{.id = "render.reload_shaders", .label = "Reload Shaders", .menuPath = "Render/Reload Shaders"}), "a command registers");
    Require(!registry.Register(Command{.id = "render.reload_shaders", .label = "Again", .menuPath = "Render/Again"}), "a taken id is refused");
    Require(!registry.Register(Command{.id = "", .label = "No Id", .menuPath = "Render/No Id"}), "an empty id is refused");
    Require(!registry.Register(Command{.id = "top", .label = "Top", .menuPath = "Top"}), "an item needs a menu");
    Require(!registry.Register(Command{.id = "deep", .label = "Deep", .menuPath = "A/B/C/Deep"}), "only one submenu level is allowed");
    Require(!registry.Register(Command{.id = "gap", .label = "Gap", .menuPath = "A//Gap"}), "empty path segments are refused");
    Require(registry.Register(Command{.id = "palette.only", .label = "Palette Only", .menuPath = ""}), "a command can be in no menu");

    Require(registry.GetCommands().size() == 2, "refused commands are not kept");
    Require(registry.Find("render.reload_shaders") != nullptr, "a command is found by id");
    Require(registry.Find("missing") == nullptr, "an unknown id is not found");

    const CommandMenuNode& root = registry.GetMenuRoot();
    Require(root.children.size() == 1, "a command with no menuPath adds no menu");
    Require(root.children[0].label == "Render", "the menu is named by the path");
    Require(root.children[0].children[0].label == "Reload Shaders", "the item is the last path segment");
}

void TestMenuTree()
{
    CommandRegistry registry;
    registry.Register(Command{.id = "file.open", .label = "Open", .menuPath = "File/Open..."});
    registry.Register(Command{.id = "render.mode.a", .label = "A", .menuPath = "Render/Mode/A"});
    registry.AddSeparator("File");
    registry.Register(Command{.id = "file.exit", .label = "Exit", .menuPath = "File/Exit"});
    registry.Register(Command{.id = "render.mode.b", .label = "B", .menuPath = "Render/Mode/B"});
    Require(!registry.AddSeparator("A/B/C"), "a separator deeper than a submenu is refused");

    const CommandMenuNode& root = registry.GetMenuRoot();
    Require(root.children.size() == 2 && root.children[0].label == "File" && root.children[1].label == "Render", "menus keep the order they first appear in");

    const CommandMenuNode* file = FindMenu(root, "File");
    Require(file->children.size() == 3, "File has open, a separator and exit");
    Require(file->children[1].kind == CommandMenuNode::Kind::Separator, "the separator sits where it was added");
    Require(registry.GetCommands()[file->children[2].commandIndex].id == "file.exit", "an item points at its command");

    const CommandMenuNode* mode = FindMenu(*FindMenu(root, "Render"), "Mode");
    Require(mode != nullptr && mode->children.size() == 2, "items with the same submenu share it");
}

void TestExecute()
{
    CommandRegistry registry;
    int runs = 0;
    bool enabled = false;
    bool checked = true;
    registry.Register(Command{
        .id = "test.run",
        .label = "Run",
        .execute =
            [&runs]
        {
            ++runs;
        },
        .isChecked =
            [&checked]
        {
            return checked;
        },
        .isEnabled =
            [&enabled]
        {
            return enabled;
        }});

    Require(!registry.Execute("test.run") && runs == 0, "a disabled command does not run");
    enabled = true;
    Require(registry.Execute("test.run") && runs == 1, "an enabled command runs");
    Require(!registry.Execute("missing"), "an unknown id does not run");

    const Command& command = *registry.Find("test.run");
    Require(IsCommandCheckable(command) && IsCommandChecked(command), "isChecked makes a command checkable");
    Require(!IsCommandCheckable(Command{.id = "plain"}) && IsCommandEnabled(Command{.id = "plain"}), "commands are enabled and not checkable by default");
}

void TestFormatShortcut()
{
    Require(FormatShortcut(0).empty(), "no shortcut formats as nothing");
    Require(FormatShortcut(ImGuiMod_Ctrl | ImGuiKey_R) == "Ctrl+R", "Ctrl+R");
    Require(FormatShortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P) == "Ctrl+Shift+P", "Ctrl+Shift+P");
    Require(FormatShortcut(ImGuiKey_F5) == "F5", "F5");
}

void TestEditorCommands()
{
    CommandRegistry registry;
    EditorCommandState state;
    bool sceneVisible = false;
    bool themeVisible = true;
    int resets = 0;
    const std::array<EditorPanel, 2> panels = {{
        {"scene", "Scene", "", &sceneVisible},
        {"theme", "Theme", "", &themeVisible},
    }};
    EditorWindowCommands window;
    window.panels = panels;
    window.resetLayout = [&resets]
    {
        ++resets;
    };
    RegisterEditorCommands(registry, state, window);

    const CommandMenuNode& root = registry.GetMenuRoot();
    const std::vector<std::string> expectedMenus = {"File", "Edit", "Scene", "View", "Render", "Tools", "Window", "Help"};
    Require(root.children.size() == expectedMenus.size(), "eight top-level menus");
    for (std::size_t index = 0; index < expectedMenus.size(); ++index)
    {
        Require(root.children[index].label == expectedMenus[index], "top-level menus are in order");
    }

    std::vector<const Command*> items;
    int maxDepth = 0;
    CollectItems(registry, root, 0, maxDepth, items);
    Require(maxDepth <= 2, "at most one submenu level");

    std::set<ImGuiKeyChord> shortcuts;
    for (const Command& command : registry.GetCommands())
    {
        if (command.shortcut != 0)
        {
            Require(shortcuts.insert(command.shortcut).second, "no two commands share a shortcut");
        }
    }
    Require(registry.Find("render.reload_shaders")->shortcut == (ImGuiMod_Ctrl | ImGuiKey_R), "Reload Shaders is Ctrl+R");
    Require(registry.Find("tools.command_palette")->shortcut == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P), "the command palette is Ctrl+Shift+P");

    // View is debug visualization, Render is the pipeline: neither holds the other's commands.
    for (const Command* command : items)
    {
        if (command->id.starts_with("view."))
        {
            Require(command->menuPath.starts_with("View/"), "view commands are in View");
        }
        if (command->id.starts_with("render."))
        {
            Require(command->menuPath.starts_with("Render/"), "render commands are in Render");
        }
    }

    // Radio groups hold one choice.
    registry.Execute("render.pipeline.hybrid");
    Require(IsCommandChecked(*registry.Find("render.pipeline.hybrid")), "the chosen pipeline is checked");
    Require(!IsCommandChecked(*registry.Find("render.pipeline.rasterization")), "the other pipelines are not");
    registry.Execute("render.pipeline.path_tracing");
    Require(IsCommandChecked(*registry.Find("render.ray_tracing")), "path tracing shows ray tracing on");
    Require(!IsCommandEnabled(*registry.Find("render.ray_tracing")), "and it cannot be turned off there");

    state.rayTracingSupported = false;
    Require(!IsCommandEnabled(*registry.Find("view.debug.bvh")), "no BVH view without ray tracing");

    // Play controls.
    Require(!IsCommandEnabled(*registry.Find("scene.step")), "step needs a paused simulation");
    registry.Execute("scene.play");
    registry.Execute("scene.pause");
    Require(IsCommandChecked(*registry.Find("scene.pause")) && IsCommandEnabled(*registry.Find("scene.step")), "paused: pause is on and step runs");

    // The Window menu is the panels it was given.
    const CommandMenuNode* windowMenu = FindMenu(root, "Window");
    Require(windowMenu->children[0].label == "Scene" && windowMenu->children[1].label == "Theme", "the Window menu lists the panels in order");
    registry.Execute("window.scene");
    registry.Execute("window.theme");
    Require(sceneVisible && !themeVisible, "panel commands toggle their panels");
    registry.Execute("window.show_all");
    Require(sceneVisible && themeVisible, "show all opens every panel");
    registry.Execute("window.reset_layout");
    Require(resets == 1, "reset layout reaches the controller");

    // The toolbar only names registered commands.
    const ToolbarLayout layout = BuildEditorToolbarLayout();
    for (const std::vector<ToolbarGroup>* section : {&layout.left, &layout.center, &layout.right})
    {
        for (const ToolbarGroup& group : *section)
        {
            for (const ToolbarItem& item : group)
            {
                Require(item.commandId.empty() || registry.Find(item.commandId) != nullptr, "toolbar buttons name commands");
                for (const std::string& id : item.dropdownCommandIds)
                {
                    Require(registry.Find(id) != nullptr, "toolbar dropdowns name commands");
                }
            }
        }
    }
}
}

int main()
{
    ImGui::CreateContext();
    try
    {
        TestRegistration();
        TestMenuTree();
        TestExecute();
        TestFormatShortcut();
        TestEditorCommands();
    }
    catch (const std::exception& error)
    {
        std::cerr << "command_registry_tests failed: " << error.what() << '\n';
        ImGui::DestroyContext();
        return 1;
    }
    ImGui::DestroyContext();
    std::cout << "command_registry_tests passed\n";
    return 0;
}
