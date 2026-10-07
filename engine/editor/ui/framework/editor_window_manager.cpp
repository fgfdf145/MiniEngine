#include "editor_window_manager.h"

#include "editor_context.h"

namespace me
{

EditorWindow* EditorWindowManager::FindById(const std::string& id) const
{
    for (const Entry& entry : m_windows)
    {
        if (entry.window->GetId() == id)
        {
            return entry.window.get();
        }
    }
    return nullptr;
}

void EditorWindowManager::Open(EditorWindow& window, bool focus)
{
    window.Open();
    if (focus)
    {
        Focus(window);
    }
}

void EditorWindowManager::Focus(const EditorWindow& window)
{
    m_focusRequest = window.GetTitle();
}

void EditorWindowManager::TickAndDraw(EditorContext& context, bool fullscreen)
{
    for (Entry& entry : m_windows)
    {
        entry.window->Tick(context);
    }
    for (Entry& entry : m_windows)
    {
        if (!fullscreen || entry.window->DrawsInFullscreen())
        {
            entry.window->Draw(context);
        }
    }
    // Whoever opened or closed it: a command, the title bar, the window itself.
    for (Entry& entry : m_windows)
    {
        const bool open = entry.window->IsOpen();
        if (open == entry.wasOpen)
        {
            continue;
        }
        entry.wasOpen = open;
        if (open)
        {
            entry.window->OnOpen(context);
        }
        else
        {
            entry.window->OnClose(context);
        }
    }
    // Once every window has been drawn, so one opened this frame exists to be focused.
    if (!fullscreen && !m_focusRequest.empty())
    {
        ImGui::SetWindowFocus(m_focusRequest.c_str());
        m_focusRequest.clear();
    }
}

std::vector<EditorPanelMenuEntry> EditorWindowManager::BuildPanelMenuEntries() const
{
    std::vector<EditorPanelMenuEntry> entries;
    entries.reserve(m_panels.size());
    for (EditorPanel* panel : m_panels)
    {
        entries.push_back({panel->GetId(), panel->GetTitle(), panel->GetIcon(), panel->OpenFlag()});
    }
    return entries;
}

void EditorWindowManager::ApplyOpenState(const EditorWindowVisibilitySettings& settings)
{
    for (EditorPanel* panel : m_panels)
    {
        const auto open = settings.open.find(panel->GetSettingsKey());
        if (open != settings.open.end())
        {
            panel->SetOpen(open->second);
        }
    }
}

void EditorWindowManager::WriteOpenState(EditorWindowVisibilitySettings& settings) const
{
    for (const EditorPanel* panel : m_panels)
    {
        settings.open[panel->GetSettingsKey()] = panel->IsOpen();
    }
}

std::vector<bool> EditorWindowManager::CapturePanelOpenState() const
{
    std::vector<bool> open;
    open.reserve(m_panels.size());
    for (const EditorPanel* panel : m_panels)
    {
        open.push_back(panel->IsOpen());
    }
    return open;
}
}
