#include "imgui_frame_snapshot.h"

#include <cstring>

namespace me
{

namespace
{
// Copies into dst's storage, keeping its capacity: ImVector's own assignment frees it first.
template <typename T>
void CopyInto(ImVector<T>& dst, const ImVector<T>& src)
{
    dst.resize(src.Size);
    if (src.Size > 0)
    {
        std::memcpy(dst.Data, src.Data, static_cast<size_t>(src.Size) * sizeof(T));
    }
}
}

ImGuiFrameSnapshot::~ImGuiFrameSnapshot()
{
    for (ImDrawList* list : m_lists)
    {
        IM_DELETE(list);
    }
}

void ImGuiFrameSnapshot::Capture(const ImDrawData& drawData)
{
    while (m_lists.size() < static_cast<size_t>(drawData.CmdListsCount))
    {
        // No shared data: the copy is only ever read.
        m_lists.push_back(IM_NEW(ImDrawList)(nullptr));
    }

    m_drawData.Clear();
    m_drawData.Valid = drawData.Valid;
    m_drawData.TotalIdxCount = drawData.TotalIdxCount;
    m_drawData.TotalVtxCount = drawData.TotalVtxCount;
    m_drawData.DisplayPos = drawData.DisplayPos;
    m_drawData.DisplaySize = drawData.DisplaySize;
    m_drawData.FramebufferScale = drawData.FramebufferScale;
    // The Vulkan backend keeps its vertex and index buffers in the viewport's RendererUserData, which
    // it sets only when it starts and stops (with the render thread idle), so the copy keeps the
    // viewport. Not the texture list: the backend makes no texture requests while drawing the copy,
    // as the main thread served them before copying (they set the IDs resolved below).
    m_drawData.OwnerViewport = drawData.OwnerViewport;
    m_drawData.Textures = nullptr;
    for (int index = 0; index < drawData.CmdListsCount; ++index)
    {
        const ImDrawList& source = *drawData.CmdLists[index];
        ImDrawList& copy = *m_lists[static_cast<size_t>(index)];
        CopyInto(copy.CmdBuffer, source.CmdBuffer);
        CopyInto(copy.IdxBuffer, source.IdxBuffer);
        CopyInto(copy.VtxBuffer, source.VtxBuffer);
        copy.Flags = source.Flags;
        for (ImDrawCmd& command : copy.CmdBuffer)
        {
            if (command.UserCallback == nullptr)
            {
                command.TexRef = ImTextureRef(command.GetTexID());
            }
        }
        m_drawData.CmdLists.push_back(&copy);
    }
    m_drawData.CmdListsCount = drawData.CmdListsCount;
    m_valid = true;
}

void ImGuiFrameSnapshot::Clear()
{
    m_drawData.Clear();
    m_valid = false;
}

void ImGuiFrameSnapshot::ReplaceTexture(ImTextureID from, ImTextureID to)
{
    for (int index = 0; index < m_drawData.CmdListsCount; ++index)
    {
        ImVector<ImDrawCmd>& commands = m_drawData.CmdLists[index]->CmdBuffer;
        for (int command = 0; command < commands.Size;)
        {
            ImDrawCmd& drawCommand = commands[command];
            if (drawCommand.UserCallback != nullptr || drawCommand.TexRef._TexID != from)
            {
                ++command;
                continue;
            }
            if (to == ImTextureID_Invalid)
            {
                // Each command carries its own offsets into the buffers, so dropping one leaves the
                // others as they were.
                commands.erase(commands.Data + command);
                continue;
            }
            drawCommand.TexRef = ImTextureRef(to);
            ++command;
        }
    }
}

ImDrawData* ImGuiFrameSnapshot::GetDrawData()
{
    return m_valid ? &m_drawData : nullptr;
}
}
