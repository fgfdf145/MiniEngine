#include "about_modal.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/core/version/engine_version.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace me
{

AboutModal::AboutModal()
    : EditorModal("about", "About MiniEngine")
{
}

void AboutModal::OnGui(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::Text("MiniEngine %s", EngineVersion::String());
    ImGui::TextUnformatted("A Vulkan renderer and scene editor.");
    ImGui::Separator();
    const int sdlVersion = SDL_GetVersion();
    ImGui::TextDisabled(
        "Dear ImGui %s, SDL %d.%d.%d",
        ImGui::GetVersion(),
        SDL_VERSIONNUM_MAJOR(sdlVersion),
        SDL_VERSIONNUM_MINOR(sdlVersion),
        SDL_VERSIONNUM_MICRO(sdlVersion));
    ImGui::TextDisabled("Project folder: %s", EnginePaths::ProjectRoot().string().c_str());
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f * UiScale(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        CloseModal();
    }
}
}
