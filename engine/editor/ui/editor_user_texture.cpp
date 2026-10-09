#include "editor_user_texture.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace me
{

namespace
{
// Textures handed back, waiting for the backend to destroy them. The backend destroys a texture
// that asks to be destroyed once it has gone unused for as many frames as the swapchain has images,
// so a frame still in flight never loses it; ImGui counts that only for its own atlas, so it is
// counted here for these.
std::vector<std::unique_ptr<ImTextureData>>& Retired()
{
    static std::vector<std::unique_ptr<ImTextureData>> retired;
    return retired;
}

void Retire(std::unique_ptr<ImTextureData> texture)
{
    if (!texture)
    {
        return;
    }
    if (ImGui::GetCurrentContext() == nullptr)
    {
        // No ImGui (shutting down): the backend has already let go of every texture.
        return;
    }
    // Without pixels a destroyed texture stays destroyed (SetStatus would ask for it again). One never
    // made on the GPU goes through the backend too, which only marks it destroyed.
    texture->DestroyPixels();
    texture->UnusedFrames = 0;
    texture->SetStatus(ImTextureStatus_WantDestroy);
    Retired().push_back(std::move(texture));
}
}

EditorUserTexture::~EditorUserTexture()
{
    Reset();
}

void EditorUserTexture::Upload(int width, int height, const uint8_t* rgba)
{
    if (width <= 0 || height <= 0 || rgba == nullptr)
    {
        return;
    }
    if (m_texture && (m_texture->Width != width || m_texture->Height != height))
    {
        Reset();
    }
    if (!m_texture)
    {
        m_texture = std::make_unique<ImTextureData>();
        m_texture->Create(ImTextureFormat_RGBA32, width, height);
        m_texture->UseColors = true;
        std::memcpy(m_texture->Pixels, rgba, static_cast<size_t>(width) * height * 4);
        ImGui::RegisterUserTexture(m_texture.get());
        return;
    }
    std::memcpy(m_texture->Pixels, rgba, static_cast<size_t>(width) * height * 4);
    // A texture the backend has not made yet takes the new pixels when it does.
    if (m_texture->Status == ImTextureStatus_WantCreate)
    {
        return;
    }
    ImTextureRect whole{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
    m_texture->UpdateRect = whole;
    m_texture->UsedRect = whole;
    m_texture->Updates.resize(0);
    m_texture->Updates.push_back(whole);
    m_texture->SetStatus(ImTextureStatus_WantUpdates);
}

void EditorUserTexture::Reset()
{
    Retire(std::move(m_texture));
}

ImTextureRef EditorUserTexture::Ref() const
{
    return m_texture ? m_texture->GetTexRef() : ImTextureRef();
}

void EditorUserTexture::CollectRetired()
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }
    std::vector<std::unique_ptr<ImTextureData>>& retired = Retired();
    for (std::unique_ptr<ImTextureData>& texture : retired)
    {
        if (texture->Status == ImTextureStatus_Destroyed)
        {
            ImGui::UnregisterUserTexture(texture.get());
            texture.reset();
        }
        else
        {
            ++texture->UnusedFrames;
        }
    }
    std::erase_if(retired, [](const std::unique_ptr<ImTextureData>& texture)
                  {
                      return texture == nullptr;
                  });
}
}
