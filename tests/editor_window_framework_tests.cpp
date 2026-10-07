// The editor UI's window framework (engine/editor/ui/framework): what EditorWindowManager does with
// the EditorWindow, EditorPanel and EditorModal it owns, frame by frame, without a GPU.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_commands.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_modal.h>
#include <engine/editor/ui/framework/editor_panel.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "imgui_software_raster.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

// Counts what the manager calls.
struct Calls
{
    int ticks = 0;
    int guis = 0;
    int opens = 0;
    int closes = 0;
};

class TestPanel final : public EditorPanel
{
  public:
    TestPanel(std::string id, std::string title, bool open, EditorDockSlot slot = EditorDockSlot::Floating)
        : EditorPanel(std::move(id), std::move(title), "", slot)
    {
        SetOpen(open);
    }

    void Tick(EditorContext&) override
    {
        ++calls.ticks;
    }
    void OnOpen(EditorContext&) override
    {
        ++calls.opens;
    }
    void OnClose(EditorContext&) override
    {
        ++calls.closes;
    }

    Calls calls;
    bool closeFromInside = false;

  protected:
    void OnGui(EditorContext&) override
    {
        ++calls.guis;
        if (closeFromInside)
        {
            Close();
        }
    }
};

// A second panel type: the manager keeps one window per type.
class OtherPanel final : public EditorPanel
{
  public:
    OtherPanel()
        : EditorPanel("other", "Other", "")
    {
    }
    // Saved under an older name, as the asset browser is.
    std::string GetSettingsKey() const override
    {
        return "legacy_other";
    }

  protected:
    void OnGui(EditorContext&) override
    {
    }
};

class TestToolWindow final : public EditorWindow
{
  public:
    TestToolWindow()
        : EditorWindow("tool", "Tool")
    {
    }
    int guis = 0;

  protected:
    void OnGui(EditorContext&) override
    {
        ++guis;
    }
};

class TestModal final : public EditorModal
{
  public:
    TestModal()
        : EditorModal("question", "Question?")
    {
    }

    void OnClose(EditorContext&) override
    {
        ++closes;
    }

    int guis = 0;
    int closes = 0;
    bool answer = false;

  protected:
    void OnGui(EditorContext& context) override
    {
        ++guis;
        if (answer)
        {
            context.result.actions.newScene = true;
            CloseModal();
        }
    }
};

// What the windows are drawn with; the panels here only count.
struct Fixture
{
    EditorScene scene;
    Camera camera;
    ViewportMatrices matrices;
    EditorFrameInput frame;
    EditorUiFrameResult result;
    EditorSharedState state;
    EditorStyle style;
    EditorWindowManager windows;
    CommandRegistry commands;

    TestPanel* first = nullptr;
    OtherPanel* other = nullptr;
    TestToolWindow* tool = nullptr;
    TestModal* modal = nullptr;

    Fixture()
    {
        first = &windows.Register<TestPanel>("first", "First", true, EditorDockSlot::Left);
        other = &windows.Register<OtherPanel>();
        tool = &windows.Register<TestToolWindow>();
        modal = &windows.Register<TestModal>();
    }

    void Frame(bool fullscreen = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        result = EditorUiFrameResult{};
        EditorContext context{scene, camera, matrices, frame, result, state, style, windows, commands};
        ImGui::NewFrame();
        windows.TickAndDraw(context, fullscreen);
        ImGui::Render();
        test::ServeTextures(*ImGui::GetDrawData());
    }
};

void TestRegistration()
{
    Fixture fixture;
    Require(fixture.windows.Find<TestPanel>() == fixture.first, "a window is found by its type");
    Require(fixture.windows.Find<TestModal>() == fixture.modal, "modals are registered like any window");
    Require(fixture.windows.FindById("tool") == fixture.tool, "a window is found by its id");
    Require(fixture.windows.FindById("missing") == nullptr, "an unknown id finds nothing");

    // Only panels are in the Window menu, in the order they were registered.
    const std::vector<EditorPanel*>& panels = fixture.windows.GetPanels();
    Require(panels.size() == 2 && panels[0] == fixture.first && panels[1] == fixture.other, "the panels keep their order");
    const std::vector<EditorPanelMenuEntry> entries = fixture.windows.BuildPanelMenuEntries();
    Require(entries.size() == 2 && entries[0].id == "first" && entries[0].windowName == "First", "each panel has a menu entry");
    Require(entries[1].visible == fixture.other->OpenFlag(), "a menu entry toggles the panel's own open flag");
    Require(fixture.first->GetDefaultDockSlot() == EditorDockSlot::Left, "a panel keeps its dock slot");
}

void TestLifecycle()
{
    Fixture fixture;
    TestPanel& panel = *fixture.first;

    fixture.Frame();
    Require(panel.calls.ticks == 1 && panel.calls.guis == 1, "an open panel is ticked and drawn");
    Require(panel.calls.opens == 1 && panel.calls.closes == 0, "a panel open from the start is told so once");
    Require(fixture.tool->guis == 0, "a closed window is not drawn");

    fixture.Frame();
    Require(panel.calls.opens == 1, "OnOpen comes once, not every frame");

    // Closed from inside its own contents: drawn this frame, told once it has been.
    panel.closeFromInside = true;
    fixture.Frame();
    Require(panel.calls.guis == 3 && panel.calls.closes == 1 && !panel.IsOpen(), "a panel that closes itself is told");
    panel.closeFromInside = false;

    fixture.Frame();
    Require(panel.calls.ticks == 4 && panel.calls.guis == 3, "a closed panel is ticked but not drawn");

    // Opened from outside, as a Window menu command does.
    *panel.OpenFlag() = true;
    fixture.Frame();
    Require(panel.calls.opens == 2 && panel.calls.guis == 4, "a panel opened by its flag is drawn and told");
}

void TestFullscreen()
{
    Fixture fixture;
    fixture.tool->Open();
    fixture.Frame(true);
    Require(fixture.first->calls.guis == 0 && fixture.tool->guis == 0, "over the fullscreen viewport only some windows draw");
    Require(fixture.first->calls.ticks == 1, "every window is ticked over the fullscreen viewport");
    fixture.Frame(false);
    Require(fixture.first->calls.guis == 1 && fixture.tool->guis == 1, "the windows come back with the editor");
}

void TestModalLifecycle()
{
    Fixture fixture;
    TestModal& modal = *fixture.modal;
    fixture.Frame();
    Require(modal.guis == 0 && !modal.IsOpen(), "a modal waits to be asked for");

    modal.Open();
    fixture.Frame();
    Require(modal.guis == 1 && modal.IsOpen(), "a modal asked for opens when next drawn");
    fixture.Frame(true);
    Require(modal.guis == 2, "a modal also shows over the fullscreen viewport");

    modal.answer = true;
    fixture.Frame();
    Require(fixture.result.actions.newScene, "a modal's answer reaches the frame's result");
    Require(modal.closes == 0, "OnClose waits until ImGui has closed the popup");
    modal.answer = false;
    fixture.Frame();
    Require(!modal.IsOpen() && modal.closes == 1 && modal.guis == 3, "a modal closed by its button is told once");
}

void TestOpenStateSettings()
{
    Fixture fixture;
    EditorWindowVisibilitySettings settings;
    settings.open["first"] = false;
    settings.open["legacy_other"] = true;
    fixture.windows.ApplyOpenState(settings);
    Require(!fixture.first->IsOpen() && fixture.other->IsOpen(), "the settings open and close panels by their keys");

    fixture.first->Open();
    EditorWindowVisibilitySettings written;
    fixture.windows.WriteOpenState(written);
    Require(written.open.size() == 2 && written.open["first"] && written.open["legacy_other"], "every panel is saved under its key");
    Require(fixture.windows.CapturePanelOpenState() == std::vector<bool>({true, true}), "the open state is captured in panel order");
}

void TestWindowCommandsAndFocus()
{
    Fixture fixture;
    const std::vector<EditorPanelMenuEntry> panels = fixture.windows.BuildPanelMenuEntries();
    EditorWindowCommands window;
    window.panels = panels;
    RegisterEditorCommands(fixture.commands, fixture.state.commands, window);
    const Command* toggle = fixture.commands.Find("window.other");
    Require(toggle != nullptr && fixture.commands.Execute(*toggle) && fixture.other->IsOpen(), "the Window menu opens a panel");
    const Command* showAll = fixture.commands.Find("window.show_all");
    fixture.first->Close();
    Require(showAll != nullptr && fixture.commands.Execute(*showAll) && fixture.first->IsOpen(), "Show All Panels opens every panel");

    // Open with focus: the window is in front once every window has been drawn.
    fixture.windows.Open<TestToolWindow>();
    fixture.Frame();
    const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow;
    Require(focused != nullptr && std::string_view(focused->Name) == "Tool", "an opened window is focused");
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    int result = 0;
    try
    {
        TestRegistration();
        TestLifecycle();
        TestFullscreen();
        TestModalLifecycle();
        TestOpenStateSettings();
        TestWindowCommandsAndFocus();
        std::cout << "editor_window_framework_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "editor_window_framework_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
