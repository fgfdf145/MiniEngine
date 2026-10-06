#include "editor_window.h"

#include "editor_context.h"
#include "editor_style.h"

#include <utility>

namespace me
{

EditorWindow::EditorWindow(std::string id, std::string title, std::string icon)
    : m_id(std::move(id)), m_title(std::move(title)), m_icon(std::move(icon))
{
}

bool EditorWindow::ShouldDraw(const EditorContext& context) const
{
    static_cast<void>(context);
    return m_open;
}

void EditorWindow::Draw(EditorContext& context)
{
    if (!ShouldDraw(context))
    {
        return;
    }
    m_uiScale = context.style.EffectiveUiScale();
    PreBegin(context);
    const bool contentsDrawn = BeginWindow(context);
    if (contentsDrawn)
    {
        OnGui(context);
    }
    EndWindow(contentsDrawn);
    PostEnd(context);
}

bool EditorWindow::BeginWindow(EditorContext& context)
{
    return ImGui::Begin(GetImGuiName(context), IsClosable(context) ? &m_open : nullptr, GetWindowFlags(context));
}

void EditorWindow::EndWindow(bool contentsDrawn)
{
    // ImGui::End pairs with every Begin, whether or not the window was visible.
    static_cast<void>(contentsDrawn);
    ImGui::End();
}
}
