#pragma once

#include <imgui.h>

#include <vector>

namespace me
{

// Textures the render thread makes and replaces on its own schedule. The editor UI names them by
// these IDs, and the render thread swaps in the descriptor set they have when it draws the frame
// (ImGuiFrameSnapshot::ReplaceTexture), so a frame on its way never names a released one.
// The scene viewport: the tone mapped image of the frame being drawn, one per swapchain image.
inline constexpr ImTextureID kViewportTextureId = 0xFFFF'FFFF'FFFF'F001ull;
// The scene's minimap picture.
inline constexpr ImTextureID kMinimapTextureId = 0xFFFF'FFFF'FFFF'F002ull;

// One frame's ImGui draw data, copied for the render thread to draw while the main thread builds
// the next frame in the same ImGui context. Every texture reference is resolved to its ID while
// copying: the font atlas's texture belongs to the context. The draw lists and their buffers are
// kept from one capture to the next.
class ImGuiFrameSnapshot
{
  public:
    ImGuiFrameSnapshot() = default;
    ~ImGuiFrameSnapshot();
    ImGuiFrameSnapshot(const ImGuiFrameSnapshot&) = delete;
    ImGuiFrameSnapshot& operator=(const ImGuiFrameSnapshot&) = delete;

    // After ImGui::Render, with the ImGui context current.
    void Capture(const ImDrawData& drawData);
    void Clear();
    // Points every draw command that samples `from` at `to` instead; with an invalid `to` the
    // commands are left out.
    void ReplaceTexture(ImTextureID from, ImTextureID to);
    // The copy, or null before the first capture or after Clear.
    ImDrawData* GetDrawData();

  private:
    ImDrawData m_drawData;
    bool m_valid = false;
    // Owned; the first m_drawData.CmdListsCount are this frame's.
    std::vector<ImDrawList*> m_lists;
};
}
