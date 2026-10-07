#pragma once

// Owns the editor's windows, like Unreal's tab manager: windows are registered once, then ticked
// and drawn every frame in registration order. Windows reach one another through it (Find, Open,
// Focus) rather than through the editor shell.

#include "editor_panel.h"
#include "editor_window.h"

#include <engine/editor/editor_commands.h>
#include <engine/editor/engine_settings.h>

#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me
{

struct EditorContext;

class EditorWindowManager
{
  public:
    EditorWindowManager() = default;
    EditorWindowManager(const EditorWindowManager&) = delete;
    EditorWindowManager& operator=(const EditorWindowManager&) = delete;

    // Creates a window of type T, one per type. Panels go into the Window menu in the order they
    // are registered.
    template <typename T, typename... Args>
    T& Register(Args&&... args)
    {
        auto window = std::make_unique<T>(std::forward<Args>(args)...);
        T& registered = *window;
        if constexpr (std::is_base_of_v<EditorPanel, T>)
        {
            m_panels.push_back(&registered);
        }
        m_byType.emplace(std::type_index(typeid(T)), &registered);
        m_windows.push_back(Entry{std::move(window), false});
        return registered;
    }

    template <typename T>
    T* Find() const
    {
        const auto found = m_byType.find(std::type_index(typeid(T)));
        return found == m_byType.end() ? nullptr : static_cast<T*>(found->second);
    }
    // A window that is always registered.
    template <typename T>
    T& Get() const
    {
        return *Find<T>();
    }
    EditorWindow* FindById(const std::string& id) const;

    // Opens the window and, once every window has been drawn this frame, brings it to the front.
    void Open(EditorWindow& window, bool focus = true);
    template <typename T>
    T& Open(bool focus = true)
    {
        T& window = Get<T>();
        Open(window, focus);
        return window;
    }
    // Brings the window to the front once every window has been drawn this frame.
    void Focus(const EditorWindow& window);

    // Every window's Tick, then every window drawn, then OnOpen and OnClose for those whose state
    // changed, then the focus request. Over the fullscreen viewport only the windows that draw there
    // are drawn, and nothing is focused.
    void TickAndDraw(EditorContext& context, bool fullscreen);

    const std::vector<EditorPanel*>& GetPanels() const
    {
        return m_panels;
    }
    // The Window menu's entries, one per panel.
    std::vector<EditorPanelMenuEntry> BuildPanelMenuEntries() const;

    // The panels' open state, kept in the editor settings under each panel's settings key. A panel
    // the settings do not name keeps its default.
    void ApplyOpenState(const EditorWindowVisibilitySettings& settings);
    void WriteOpenState(EditorWindowVisibilitySettings& settings) const;
    // Every panel's open state, to tell whether one opened or closed across a frame.
    std::vector<bool> CapturePanelOpenState() const;

  private:
    struct Entry
    {
        std::unique_ptr<EditorWindow> window;
        bool wasOpen = false;
    };

    std::vector<Entry> m_windows;
    std::vector<EditorPanel*> m_panels;
    std::unordered_map<std::type_index, EditorWindow*> m_byType;
    std::string m_focusRequest;
};
}
