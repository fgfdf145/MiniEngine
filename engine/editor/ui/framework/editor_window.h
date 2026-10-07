#pragma once

// The base of every top-level window the editor draws, in the way of Unity's EditorWindow or
// Unreal's SDockTab: a window has an id, a title and an open state, and the EditorWindowManager
// ticks it, draws it and tells it when it opens and closes.
//
//   EditorWindow                 any top-level window; floating tool windows derive from it directly
//   +- EditorPanel               a dockable panel listed in the Window menu, its open state saved
//   +- EditorModal               a modal popup, opened on request and closed by its own buttons
//
// Draw() is a template method: it begins the window (BeginWindow), lets the subclass fill it
// (OnGui) and ends it (EndWindow). Subclasses override the hooks, never Draw itself.

#include <imgui.h>

#include <string>

namespace me
{

struct EditorContext;

class EditorWindow
{
  public:
    EditorWindow(std::string id, std::string title, std::string icon = {});
    virtual ~EditorWindow() = default;
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // A stable name for commands and settings, e.g. "graphics_debug".
    const std::string& GetId() const
    {
        return m_id;
    }
    // The ImGui window's name, which the dock layout and focus requests refer to.
    const std::string& GetTitle() const
    {
        return m_title;
    }
    // An icon font glyph for menus; may be empty.
    const std::string& GetIcon() const
    {
        return m_icon;
    }

    bool IsOpen() const
    {
        return m_open;
    }
    // Opening or closing takes effect when the window is next drawn; the manager calls OnOpen and
    // OnClose once it sees the change, whoever made it (a command, the title bar's X, the window).
    virtual void Open()
    {
        m_open = true;
    }
    void Close()
    {
        m_open = false;
    }
    void SetOpen(bool open)
    {
        if (open)
        {
            Open();
        }
        else
        {
            Close();
        }
    }
    // For ImGui's p_open and the Window menu's toggles.
    bool* OpenFlag()
    {
        return &m_open;
    }

    // Called every frame for every window before any is drawn, open or not: work that must go on
    // while the window is hidden.
    virtual void Tick(EditorContext& context)
    {
        static_cast<void>(context);
    }
    virtual void OnOpen(EditorContext& context)
    {
        static_cast<void>(context);
    }
    virtual void OnClose(EditorContext& context)
    {
        static_cast<void>(context);
    }

    // Whether Draw does anything this frame. Open windows by default.
    virtual bool ShouldDraw(const EditorContext& context) const;
    // Whether the window is drawn over the fullscreen viewport, where everything else is hidden.
    virtual bool DrawsInFullscreen() const
    {
        return false;
    }

    // Begins the window, fills it and ends it. The manager calls it once a frame.
    void Draw(EditorContext& context);

  protected:
    // The window's contents. Called between BeginWindow and EndWindow, only when the window is
    // visible (not collapsed or a hidden tab).
    virtual void OnGui(EditorContext& context) = 0;

    // Before the window begins: SetNextWindow* calls and style pushes.
    virtual void PreBegin(EditorContext& context)
    {
        static_cast<void>(context);
    }
    // After the window ends: undoes what PreBegin pushed.
    virtual void PostEnd(EditorContext& context)
    {
        static_cast<void>(context);
    }
    virtual ImGuiWindowFlags GetWindowFlags(const EditorContext& context) const
    {
        static_cast<void>(context);
        return ImGuiWindowFlags_None;
    }
    // The name passed to ImGui::Begin; the title unless the window draws as something else.
    virtual const char* GetImGuiName(const EditorContext& context) const
    {
        static_cast<void>(context);
        return m_title.c_str();
    }
    // Whether the title bar has a close button.
    virtual bool IsClosable(const EditorContext& context) const
    {
        static_cast<void>(context);
        return true;
    }

    // How the window begins and ends: an ImGui window here, a popup for EditorModal. BeginWindow
    // returns whether the contents are to be drawn; EndWindow gets that answer.
    virtual bool BeginWindow(EditorContext& context);
    virtual void EndWindow(bool contentsDrawn);

    // The editor's effective UI scale (DPI times the user's multiplier), as of this Draw.
    float UiScale() const
    {
        return m_uiScale;
    }

  private:
    std::string m_id;
    std::string m_title;
    std::string m_icon;
    bool m_open = false;
    float m_uiScale = 1.0f;
};
}
