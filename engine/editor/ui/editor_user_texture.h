#pragma once

#include <imgui.h>

#include <cstdint>
#include <memory>

namespace me
{

// An RGBA8 image the editor makes on the CPU and shows through ImGui (the Material Editor's preview
// and texture thumbnails): one of ImGui 1.92's user textures, which the renderer backend creates and
// updates when ImGui asks (VulkanRenderer::ApplyImGuiTextureRequests). A texture of another size
// replaces it; the old one is handed back for the backend to destroy once no frame in flight draws
// it, and only then freed (CollectRetired).
class EditorUserTexture
{
  public:
    EditorUserTexture() = default;
    EditorUserTexture(const EditorUserTexture&) = delete;
    EditorUserTexture& operator=(const EditorUserTexture&) = delete;
    ~EditorUserTexture();

    // Copies the pixels (rows top-down) into the texture and asks for them to be uploaded.
    void Upload(int width, int height, const uint8_t* rgba);
    // Hands the texture back (it is destroyed once unused).
    void Reset();

    bool IsValid() const
    {
        return m_texture != nullptr;
    }
    int Width() const
    {
        return m_texture ? m_texture->Width : 0;
    }
    int Height() const
    {
        return m_texture ? m_texture->Height : 0;
    }
    // For ImGui::Image and ImDrawList::AddImage; an empty reference without a texture.
    ImTextureRef Ref() const;

    // Advances the textures handed back toward their destruction and frees those the backend has
    // destroyed. Once a frame, while an ImGui context exists.
    static void CollectRetired();

  private:
    std::unique_ptr<ImTextureData> m_texture;
};
}
