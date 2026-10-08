#include "imgui_software_raster.h"

#include <engine/editor/imgui_frame_snapshot.h>

#include <imgui.h>

#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

constexpr ImTextureID kOtherTexture = 0x1234;

// One frame: some text (the font atlas), the viewport image and another image.
ImDrawData& DrawFrame(float x)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(x, 10.0f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
    ImGui::Begin("Panel");
    ImGui::TextUnformatted("Hello");
    ImGui::Image(ImTextureRef(kViewportTextureId), ImVec2(64.0f, 64.0f));
    ImGui::Image(ImTextureRef(kOtherTexture), ImVec2(32.0f, 32.0f));
    ImGui::End();
    ImGui::Render();
    ImDrawData& drawData = *ImGui::GetDrawData();
    test::ServeTextures(drawData);
    return drawData;
}

size_t CountCommands(ImDrawData& drawData, ImTextureID texture)
{
    size_t count = 0;
    for (int list = 0; list < drawData.CmdListsCount; ++list)
    {
        for (const ImDrawCmd& command : drawData.CmdLists[list]->CmdBuffer)
        {
            if (command.UserCallback == nullptr && command.TexRef._TexData == nullptr && command.TexRef._TexID == texture)
            {
                ++count;
            }
        }
    }
    return count;
}

void CopiesTheDrawData()
{
    ImDrawData& source = DrawFrame(10.0f);
    ImGuiFrameSnapshot snapshot;
    Require(snapshot.GetDrawData() == nullptr, "nothing before the first capture");
    snapshot.Capture(source);
    ImDrawData* copy = snapshot.GetDrawData();
    Require(copy != nullptr, "a capture has draw data");
    Require(copy->CmdListsCount == source.CmdListsCount, "every draw list is copied");
    Require(copy->TotalVtxCount == source.TotalVtxCount && copy->TotalIdxCount == source.TotalIdxCount, "the counts are copied");
    for (int list = 0; list < source.CmdListsCount; ++list)
    {
        Require(copy->CmdLists[list] != source.CmdLists[list], "the lists are copies, not the context's");
        Require(copy->CmdLists[list]->VtxBuffer.Size == source.CmdLists[list]->VtxBuffer.Size, "the vertices are copied");
        Require(copy->CmdLists[list]->CmdBuffer.Size == source.CmdLists[list]->CmdBuffer.Size, "the commands are copied");
    }
    // Every reference resolved to an ID: nothing points into the context's textures.
    for (int list = 0; list < copy->CmdListsCount; ++list)
    {
        for (const ImDrawCmd& command : copy->CmdLists[list]->CmdBuffer)
        {
            Require(command.TexRef._TexData == nullptr, "a texture reference left pointing into the context");
        }
    }
    Require(CountCommands(*copy, kViewportTextureId) == 1, "the viewport image is one command");

    // The next frame in the context changes nothing in the copy.
    const ImVec2 firstVertex = copy->CmdLists[0]->VtxBuffer[0].pos;
    DrawFrame(200.0f);
    Require(copy->CmdLists[0]->VtxBuffer[0].pos.x == firstVertex.x, "the copy must not follow the context");

    // A capture reuses the copy's lists.
    ImDrawList* const list = copy->CmdLists[0];
    snapshot.Capture(*ImGui::GetDrawData());
    Require(snapshot.GetDrawData()->CmdLists[0] == list, "the draw lists are kept from one capture to the next");
}

void ReplacesTextures()
{
    ImGuiFrameSnapshot snapshot;
    snapshot.Capture(DrawFrame(10.0f));
    ImDrawData& copy = *snapshot.GetDrawData();
    const size_t before = CountCommands(copy, kOtherTexture);
    snapshot.ReplaceTexture(kViewportTextureId, 0xBEEF);
    Require(CountCommands(copy, kViewportTextureId) == 0, "the viewport ID is replaced");
    Require(CountCommands(copy, 0xBEEF) == 1, "by the descriptor set");
    Require(CountCommands(copy, kOtherTexture) == before, "other textures stay");

    // No texture to replace with: the commands go, and the rest still index their own vertices.
    int commandsBefore = 0;
    for (int list = 0; list < copy.CmdListsCount; ++list)
    {
        commandsBefore += copy.CmdLists[list]->CmdBuffer.Size;
    }
    snapshot.ReplaceTexture(kOtherTexture, ImTextureID_Invalid);
    int commandsAfter = 0;
    for (int list = 0; list < copy.CmdListsCount; ++list)
    {
        commandsAfter += copy.CmdLists[list]->CmdBuffer.Size;
    }
    Require(CountCommands(copy, kOtherTexture) == 0, "the commands of a missing texture are left out");
    Require(commandsAfter == commandsBefore - static_cast<int>(before), "only those commands go");
    Require(CountCommands(copy, 0xBEEF) == 1, "the others stay");
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    int result = 0;
    try
    {
        CopiesTheDrawData();
        ReplacesTextures();
        std::cout << "imgui frame snapshot tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
