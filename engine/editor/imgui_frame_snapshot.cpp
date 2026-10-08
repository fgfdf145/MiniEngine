#include "imgui_frame_snapshot.h"

#include <algorithm>
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

ImGuiCommandQuad CommandQuad(const ImDrawList& list, const ImDrawCmd& command)
{
    ImGuiCommandQuad quad;
    bool first = true;
    float minSum = 0.0f;
    float maxSum = 0.0f;
    for (unsigned int element = 0; element < command.ElemCount; ++element)
    {
        const ImDrawIdx index = list.IdxBuffer[static_cast<int>(command.IdxOffset + element)];
        const ImDrawVert& vertex = list.VtxBuffer[static_cast<int>(command.VtxOffset + index)];
        const float sum = vertex.pos.x + vertex.pos.y;
        if (first)
        {
            quad.min = quad.max = vertex.pos;
            quad.uvMin = quad.uvMax = vertex.uv;
            minSum = maxSum = sum;
            first = false;
            continue;
        }
        quad.min = ImVec2(std::min(quad.min.x, vertex.pos.x), std::min(quad.min.y, vertex.pos.y));
        quad.max = ImVec2(std::max(quad.max.x, vertex.pos.x), std::max(quad.max.y, vertex.pos.y));
        // An axis-aligned quad's top-left corner has the smallest x + y, its bottom-right the largest.
        if (sum < minSum)
        {
            minSum = sum;
            quad.uvMin = vertex.uv;
        }
        if (sum > maxSum)
        {
            maxSum = sum;
            quad.uvMax = vertex.uv;
        }
    }
    return quad;
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
std::optional<ImGuiCommandQuad> ImGuiFrameSnapshot::ReplaceTextureWithCallback(
    ImTextureID from,
    ImDrawCallback callback,
    void* userData,
    ImDrawCallback resetRenderState)
{
    std::optional<ImGuiCommandQuad> firstQuad;
    for (int index = 0; index < m_drawData.CmdListsCount; ++index)
    {
        ImDrawList& list = *m_drawData.CmdLists[index];
        ImVector<ImDrawCmd>& commands = list.CmdBuffer;
        for (int command = 0; command < commands.Size; ++command)
        {
            ImDrawCmd& drawCommand = commands[command];
            if (drawCommand.UserCallback != nullptr || drawCommand.TexRef._TexID != from)
            {
                continue;
            }
            if (!firstQuad.has_value())
            {
                firstQuad = CommandQuad(list, drawCommand);
            }
            drawCommand.UserCallback = callback;
            drawCommand.UserCallbackData = userData;
            drawCommand.UserCallbackDataSize = 0;
            drawCommand.UserCallbackDataOffset = -1;
            ImDrawCmd reset = drawCommand;
            reset.UserCallback = resetRenderState;
            reset.UserCallbackData = nullptr;
            reset.ElemCount = 0;
            // Inserting may move the buffer: drawCommand is not used past here.
            commands.insert(commands.Data + command + 1, reset);
            ++command;
        }
    }
    return firstQuad;
}

ImDrawData* ImGuiFrameSnapshot::GetDrawData()
{
    return m_valid ? &m_drawData : nullptr;
}
}
