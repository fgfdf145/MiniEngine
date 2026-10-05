// The Assets window at three widths, drawn without a GPU: the toolbar and the breadcrumb wrap
// instead of running past the right edge, the tiles fill each row, and the preview panel wraps
// a long path. With MINIENGINE_UI_SNAPSHOT_DIR set the picture is written there as a PNG.

#include <engine/asset/asset_manager.h>
#include <engine/asset/asset_registry.h>
#include <engine/editor/editor_icons.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me;

namespace
{
constexpr int kWidth = 1240;
constexpr int kHeight = 560;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

void ServeTextures(ImDrawData& drawData)
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

// ImGui's triangles into an RGB image (as the vehicle overlay test does).
std::vector<float> Rasterise(const ImDrawData& drawData)
{
    std::vector<float> image(static_cast<size_t>(kWidth) * kHeight * 3, 0.08f);
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
            const int clipX1 = std::min(kWidth, static_cast<int>(std::ceil(cmd.ClipRect.z)));
            const int clipY1 = std::min(kHeight, static_cast<int>(std::ceil(cmd.ClipRect.w)));
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
                        float* out = &image[(static_cast<size_t>(y) * kWidth + x) * 3];
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

void WritePng(const std::vector<float>& image, const std::filesystem::path& path)
{
    std::vector<unsigned char> bytes(image.size());
    for (size_t i = 0; i < image.size(); ++i)
    {
        bytes[i] = static_cast<unsigned char>(std::clamp(image[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    Require(stbi_write_png(path.string().c_str(), kWidth, kHeight, 3, bytes.data(), kWidth * 3) != 0, "writes " + path.string());
}

void Touch(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

struct BrowserWindow
{
    const char* name;
    ImVec2 pos;
    ImVec2 size;
    std::unique_ptr<AssetManager> manager;
};

// One frame of every window, with the mouse where `mouse` says.
ImDrawData* DrawFrame(std::vector<BrowserWindow>& windows, ImVec2 mouse, bool mouseDown)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    for (BrowserWindow& window : windows)
    {
        ImGui::SetNextWindowPos(window.pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(window.size, ImGuiCond_Always);
        if (ImGui::Begin(window.name, nullptr, ImGuiWindowFlags_NoSavedSettings))
        {
            window.manager->Draw();
        }
        ImGui::End();
    }
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    return drawData;
}

// The child window of `parent` whose name ends with `suffix`.
ImGuiWindow* FindChild(const char* parent, const char* suffix)
{
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        const std::string name = window->Name;
        if (name.starts_with(std::string(parent) + "/") && name.find(suffix) != std::string::npos)
        {
            return window;
        }
    }
    return nullptr;
}

void TestTheWindowFollowsItsWidth()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "miniengine_asset_browser_window_test" / "assets";
    std::filesystem::remove_all(root.parent_path());
    // Deep enough that the breadcrumb cannot fit the narrow window on one line.
    const std::filesystem::path deep = root / "vehicles" / "skyline_r34_vspec" / "textures_and_materials";
    Touch(deep / "a_really_long_model_name_that_wraps.glb");
    Touch(deep / "body.material.yaml");
    Touch(deep / "grove_street.yaml");
    Touch(deep / "body_diffuse.png");
    Touch(deep / "body.bin");
    std::filesystem::create_directories(deep / "lod");
    AssetRegistry::Initialize(root);

    std::vector<BrowserWindow> windows;
    windows.push_back({"Narrow", ImVec2(10.0f, 10.0f), ImVec2(230.0f, 540.0f), std::make_unique<AssetManager>(root)});
    windows.push_back({"Medium", ImVec2(250.0f, 10.0f), ImVec2(400.0f, 540.0f), std::make_unique<AssetManager>(root)});
    windows.push_back({"Wide", ImVec2(660.0f, 10.0f), ImVec2(570.0f, 540.0f), std::make_unique<AssetManager>(root)});
    for (BrowserWindow& window : windows)
    {
        window.manager->NavigateTo(deep);
    }

    const ImVec2 away(-100.0f, -100.0f);
    for (int frame = 0; frame < 3; ++frame)
    {
        DrawFrame(windows, away, false);
    }
    // Select the long-named model in each window: it is the second tile (the folder comes first).
    for (BrowserWindow& window : windows)
    {
        ImGuiWindow* list = FindChild(window.name, "##asset_list");
        Require(list != nullptr, std::string("the ") + window.name + " window has a tile list");
        const ImGuiStyle& style = ImGui::GetStyle();
        const float rowWidth = list->ContentRegionRect.GetWidth();
        const int columns = std::max(1, static_cast<int>((rowWidth + style.ItemSpacing.x) / (96.0f + style.ItemSpacing.x)));
        const float tileWidth = (rowWidth - style.ItemSpacing.x * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const ImVec2 tile = columns > 1 ? ImVec2(list->ContentRegionRect.Min.x + tileWidth * 1.5f + style.ItemSpacing.x,
                                                 list->ContentRegionRect.Min.y + 50.0f)
                                        : ImVec2(list->ContentRegionRect.Min.x + tileWidth * 0.5f,
                                                 list->ContentRegionRect.Min.y + 100.0f + style.ItemSpacing.y + 50.0f);
        DrawFrame(windows, tile, false);
        DrawFrame(windows, tile, true);
        DrawFrame(windows, tile, false);
    }
    ImDrawData* drawData = nullptr;
    for (int frame = 0; frame < 3; ++frame)
    {
        drawData = DrawFrame(windows, away, false);
    }

    if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
    {
        std::filesystem::create_directories(folder);
        WritePng(Rasterise(*drawData), std::filesystem::path(folder) / "asset_browser_widths.png");
    }
    for (const BrowserWindow& window : windows)
    {
        ImGuiWindow* imguiWindow = ImGui::FindWindowByName(window.name);
        Require(imguiWindow != nullptr, std::string("the ") + window.name + " window exists");
        const float overflow = imguiWindow->ContentSizeIdeal.x - imguiWindow->ContentRegionRect.GetWidth();
        std::cout << window.name << ": content " << imguiWindow->ContentSizeIdeal.x << " px in "
                  << imguiWindow->ContentRegionRect.GetWidth() << " px\n";
        Require(overflow <= 1.0f, std::string("nothing in the ") + window.name + " window runs past its right edge");
        Require(!imguiWindow->ScrollbarX && !imguiWindow->ScrollbarY, std::string("the ") + window.name + " window needs no scroll bar");

        // A selectable's hit box reaches half the item spacing past the tile, which ImGui counts
        // as content: the last tile of a row may stick out by that much and no more.
        ImGuiWindow* list = FindChild(window.name, "##asset_list");
        Require(list != nullptr, std::string("the ") + window.name + " window has a tile list");
        std::cout << "  tiles " << list->ContentSizeIdeal.x << " px in " << list->ContentRegionRect.GetWidth() << " px\n";
        Require(list->ContentSizeIdeal.x <= list->ContentRegionRect.GetWidth() + ImGui::GetStyle().ItemSpacing.x * 0.5f + 1.0f,
                std::string("the ") + window.name + " tiles fit their row");
        ImGuiWindow* preview = FindChild(window.name, "##asset_preview");
        Require(preview != nullptr && preview->ContentSizeIdeal.x <= preview->ContentRegionRect.GetWidth() + 1.0f,
                std::string("the ") + window.name + " preview wraps its text");
    }

    std::filesystem::remove_all(root.parent_path());
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    MergeEditorIconFont(*io.Fonts, 13.0f);
    int result = 0;
    try
    {
        TestTheWindowFollowsItsWidth();
        std::cout << "asset browser window tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset browser window tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
