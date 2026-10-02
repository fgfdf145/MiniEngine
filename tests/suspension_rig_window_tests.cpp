// The editor's Suspension Rigs window, driven without a GPU: ImGui and ImPlot run headless, the
// window loads the GT-R from the selected entity's model data, runs the rigs and draws every tab.
// With MINIENGINE_UI_SNAPSHOT_DIR set, each tab is also rasterised in software to a PNG there, to
// look at.

#include "gtr_car_spec.h"

#include <engine/asset/model_cache.h>
#include <engine/editor/ui/editor_suspension_rigs.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>
#include <implot.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace me;

namespace
{
void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

constexpr int kWidth = 1400;
constexpr int kHeight = 900;

// The textures ImGui asks for (the font atlas): kept where ImGui has them, the id is the texture.
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

// ImGui's triangles into an RGBA image: edge functions, colours and UVs interpolated, the texture
// sampled at the nearest texel, blended over.
std::vector<float> Rasterise(const ImDrawData& drawData)
{
    std::vector<float> image(static_cast<size_t>(kWidth) * kHeight * 3, 0.08f);
    const auto colour = [](ImU32 c, int shift) {
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
            const int clipX0 = std::max(0, static_cast<int>(cmd.ClipRect.x - drawData.DisplayPos.x));
            const int clipY0 = std::max(0, static_cast<int>(cmd.ClipRect.y - drawData.DisplayPos.y));
            const int clipX1 = std::min(kWidth, static_cast<int>(std::ceil(cmd.ClipRect.z - drawData.DisplayPos.x)));
            const int clipY1 = std::min(kHeight, static_cast<int>(std::ceil(cmd.ClipRect.w - drawData.DisplayPos.y)));
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
                        const float px = x + 0.5f - drawData.DisplayPos.x;
                        const float py = y + 0.5f - drawData.DisplayPos.y;
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

// One editor frame with only the rigs window in it, the window filling the display.
void Frame(SuspensionRigWindow& window, const EditorScene& scene, const char* snapshot = nullptr)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    bool open = true;
    window.Draw(scene, &open);
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR");
    if (snapshot != nullptr && folder != nullptr)
    {
        std::filesystem::create_directories(folder);
        WritePng(Rasterise(*drawData), std::filesystem::path(folder) / snapshot);
    }
}

void Frames(SuspensionRigWindow& window, const EditorScene& scene, int count, const char* snapshot = nullptr)
{
    for (int i = 0; i < count; ++i)
    {
        Frame(window, scene, i + 1 == count ? snapshot : nullptr);
    }
}

void TestWindowRunsTheRigsOnTheSelectedCar()
{
    // The GT-R as its imported model carries it.
    VehicleCarSpec spec = test::MakeGtrSpec();
    spec.wheelbase = 2.78f;
    spec.frontWeightShare = 0.555f;
    spec.frontSuspension->centerOfMassAboveWheel = -0.075f;
    spec.rearSuspension->centerOfMassAboveWheel = -0.075f;
    auto model = std::make_shared<LoadedModelData>();
    model->carSpec = spec;
    const std::string path = "suspension_rig_window_tests/nissan_gtr_gt3.gltf";
    ModelCache::Store(path, model);

    EditorScene scene;
    SerializedEntityData car;
    car.tagName = "Nissan GT-R GT3";
    car.modelSourcePath = path;
    scene.SetSelectedEntity(scene.CreateEntity(car));

    SuspensionRigWindow window;
    // The selected car loads by itself; the linkage view draws at rest and posed.
    Frames(window, scene, 3, "rigs_linkage_rest.png");
    window.SetLinkagePose(0, 35.0f, 2.0f, 0.6f);
    Frames(window, scene, 3, "rigs_linkage_front_posed.png");
    window.SetLinkagePose(1, -30.0f, -2.5f, 0.0f);
    Frames(window, scene, 3, "rigs_linkage_rear_posed.png");

    // A quick run (short sweeps and roads, no friction comparison) on the worker thread.
    suspension::RigReportOptions& options = window.Options();
    options.sweepCycles = 30;
    options.roadSeconds = 3.0;
    options.frictionComparison = false;
    window.StartRun();
    Require(window.IsRunning(), "the run starts");
    const auto start = std::chrono::steady_clock::now();
    bool snappedProgress = false;
    while (window.IsRunning())
    {
        Frame(window, scene, snappedProgress ? nullptr : "rigs_running.png");
        snappedProgress = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(120), "the run finishes");
    }
    Require(window.HasReport(), "the run leaves its report");

    window.RequestTab(SuspensionRigWindow::SummaryTab);
    Frames(window, scene, 3, "rigs_summary.png");
    window.RequestTab(SuspensionRigWindow::KcTab);
    Frames(window, scene, 3, "rigs_kc.png");
    window.RequestTab(SuspensionRigWindow::SevenPostTab);
    Frames(window, scene, 3, "rigs_seven_post.png");
    window.RequestTab(SuspensionRigWindow::LinkageTab);
    window.SetLinkagePose(0, 0.0f, 0.0f, 0.0f);
    Frames(window, scene, 3);
    ModelCache::Invalidate(path);
}
}

int main()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    int result = 0;
    try
    {
        TestWindowRunsTheRigsOnTheSelectedCar();
        std::cout << "suspension rig window tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "suspension rig window tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return result;
}
