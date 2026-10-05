#pragma once

// ImGui draw data rendered in software, for tests that look at editor panels without a GPU
// (io.BackendFlags must include RendererHasTextures; ServeTextures hands ImGui the textures).

#include <imgui.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace me::test
{
inline void ServeTextures(ImDrawData& drawData)
{
    if (drawData.Textures == nullptr)
    {
        return;
    }
    for (ImTextureData* texture : *drawData.Textures)
    {
        if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
        {
            texture->SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
            texture->SetStatus(ImTextureStatus_OK);
        }
        else if (texture->Status == ImTextureStatus_WantDestroy)
        {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// ImGui's triangles into a width x height RGB image (as the vehicle overlay test does): edge
// functions, colours interpolated, textures sampled at the nearest texel, over a dark ground.
inline std::vector<float> Rasterise(const ImDrawData& drawData, int width, int height)
{
    std::vector<float> image(static_cast<size_t>(width) * height * 3, 0.08f);
    const auto colour = [](ImU32 c, int shift)
    {
        return static_cast<float>((c >> shift) & 0xFF) / 255.0f;
    };
    for (const ImDrawList* list : drawData.CmdLists)
    {
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr || cmd.ElemCount == 0)
            {
                continue;
            }
            const ImTextureData* texture = reinterpret_cast<const ImTextureData*>(static_cast<std::uintptr_t>(cmd.GetTexID()));
            const int clipX0 = std::max(0, static_cast<int>(cmd.ClipRect.x));
            const int clipY0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
            const int clipX1 = std::min(width, static_cast<int>(std::ceil(cmd.ClipRect.z)));
            const int clipY1 = std::min(height, static_cast<int>(std::ceil(cmd.ClipRect.w)));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
            {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k)
                {
                    v[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + k]];
                }
                const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[1]->pos.y - v[0]->pos.y) * (v[2]->pos.x - v[0]->pos.x);
                if (std::abs(area) < 1e-8f)
                {
                    continue;
                }
                const int x0 = std::max(clipX0, static_cast<int>(std::floor(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))));
                const int y0 = std::max(clipY0, static_cast<int>(std::floor(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))));
                const int x1 = std::min(clipX1, static_cast<int>(std::ceil(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))));
                const int y1 = std::min(clipY1, static_cast<int>(std::ceil(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))));
                for (int y = y0; y < y1; ++y)
                {
                    for (int x = x0; x < x1; ++x)
                    {
                        const float px = x + 0.5f;
                        const float py = y + 0.5f;
                        float w[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            const ImDrawVert* a = v[(k + 1) % 3];
                            const ImDrawVert* b = v[(k + 2) % 3];
                            w[k] = ((b->pos.x - a->pos.x) * (py - a->pos.y) - (b->pos.y - a->pos.y) * (px - a->pos.x)) / area;
                        }
                        if (w[0] < 0.0f || w[1] < 0.0f || w[2] < 0.0f)
                        {
                            continue;
                        }
                        float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        float u = 0.0f;
                        float t = 0.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            rgba[0] += w[k] * colour(v[k]->col, IM_COL32_R_SHIFT);
                            rgba[1] += w[k] * colour(v[k]->col, IM_COL32_G_SHIFT);
                            rgba[2] += w[k] * colour(v[k]->col, IM_COL32_B_SHIFT);
                            rgba[3] += w[k] * colour(v[k]->col, IM_COL32_A_SHIFT);
                            u += w[k] * v[k]->uv.x;
                            t += w[k] * v[k]->uv.y;
                        }
                        if (texture != nullptr && texture->Pixels != nullptr)
                        {
                            const int tx = std::clamp(static_cast<int>(u * texture->Width), 0, texture->Width - 1);
                            const int ty = std::clamp(static_cast<int>(t * texture->Height), 0, texture->Height - 1);
                            const unsigned char* texel = texture->Pixels + (static_cast<size_t>(ty) * texture->Width + tx) * texture->BytesPerPixel;
                            if (texture->Format == ImTextureFormat_RGBA32)
                            {
                                for (int c = 0; c < 4; ++c)
                                {
                                    rgba[c] *= texel[c] / 255.0f;
                                }
                            }
                            else
                            {
                                rgba[3] *= texel[0] / 255.0f;
                            }
                        }
                        float* out = &image[(static_cast<size_t>(y) * width + x) * 3];
                        for (int c = 0; c < 3; ++c)
                        {
                            out[c] = out[c] * (1.0f - rgba[3]) + rgba[c] * rgba[3];
                        }
                    }
                }
            }
        }
    }
    return image;
}

inline void WritePng(const std::vector<float>& image, int width, int height, const std::filesystem::path& path)
{
    std::vector<unsigned char> bytes(image.size());
    for (size_t i = 0; i < image.size(); ++i)
    {
        bytes[i] = static_cast<unsigned char>(std::clamp(image[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    if (stbi_write_png(path.string().c_str(), width, height, 3, bytes.data(), width * 3) == 0)
    {
        throw std::runtime_error("cannot write " + path.string());
    }
}
}
